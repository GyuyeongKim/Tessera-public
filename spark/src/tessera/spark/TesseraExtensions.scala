package tessera.spark

import java.io.ByteArrayOutputStream

import scala.collection.mutable.ArrayBuffer

import org.apache.spark.rdd.RDD
import org.apache.spark.sql.SparkSessionExtensions
import org.apache.spark.sql.catalyst.InternalRow
import org.apache.spark.sql.catalyst.expressions._
import org.apache.spark.sql.catalyst.expressions.aggregate.{AggregateExpression, Complete, Count, Sum}
import org.apache.spark.sql.catalyst.plans.Inner
import org.apache.spark.sql.catalyst.plans.logical.{Aggregate, Join, LogicalPlan, Project}
import org.apache.spark.sql.execution.{BinaryExecNode, SparkPlan, SparkStrategy, UnaryExecNode}
import org.apache.spark.sql.execution.adaptive.LogicalQueryStage
import org.apache.spark.sql.internal.SQLConf
import org.apache.spark.sql.types._
import org.apache.spark.unsafe.types.UTF8String

class TesseraExtensions extends (SparkSessionExtensions => Unit) {
  override def apply(e: SparkSessionExtensions): Unit = e.injectPlannerStrategy(_ => TesseraStrategy)
}

case class TesseraSpec(kinds: Seq[Int], kIdx: Int, xIdx: Int, vIdx: Int, kType: DataType, xType: DataType,
                       vType: DataType, aggType: DataType, mode: Int, col0Smaller: Boolean, simMeasure: Int = 0,
                       minSim: Double = 0.0)

object TesseraStrategy extends SparkStrategy with PredicateHelper {

  override def apply(plan: LogicalPlan): Seq[SparkPlan] = plan match {
    case n: TesseraSimNode => Seq(TesseraExec(n.output, n.spec, planLater(n.child)))
    case a: Aggregate if SQLConf.get.getConfString("spark.tessera.enabled", "true").toBoolean =>
      matchAgg(a).map { case (spec, left) => TesseraExec(a.output, spec, planLater(left)): SparkPlan }
        .orElse(TesseraCross.matchCross(a).map { case (spec, xp, yp) => TesseraCrossExec(a.output, spec, planLater(xp), planLater(yp)) })
        .toSeq
    case _ => Nil
  }

  private def isInt(t: DataType): Boolean = t == IntegerType || t == LongType || t == ShortType

  private def unstage(p: LogicalPlan): LogicalPlan = p match {
    case s: LogicalQueryStage => s.logicalPlan
    case other => other
  }
  def sameInput(l: LogicalPlan, r: LogicalPlan): Boolean = unstage(l).sameResult(unstage(r))

  private def isKey(t: DataType): Boolean = isInt(t) || t == StringType

  private def pos(e: Expression, l: LogicalPlan, r: LogicalPlan): Option[(Int, Int)] = e match {
    case a: AttributeReference =>
      val i = l.output.indexWhere(_.exprId == a.exprId)
      val j = r.output.indexWhere(_.exprId == a.exprId)
      if (i >= 0) Some((i, 0)) else if (j >= 0) Some((j, 1)) else None
    case _ => None
  }

  private def pair(a: Expression, b: Expression, l: LogicalPlan, r: LogicalPlan): Option[(Int, Int)] =
    (pos(a, l, r), pos(b, l, r)) match {
      case (Some((i, s)), Some((j, t))) if i == j && s != t => Some((i, s))
      case _ => None
    }

  private def throughProjects(p: LogicalPlan): LogicalPlan = p match {
    case Project(list, c) if list.forall(_.isInstanceOf[AttributeReference]) => throughProjects(c)
    case other => other
  }

  private def matchAgg(agg: Aggregate): Option[(TesseraSpec, LogicalPlan)] = throughProjects(agg.child) match {
    case j: Join if j.joinType == Inner && j.condition.isDefined && sameInput(j.left, j.right) =>
      val (l, r) = (j.left, j.right)
      if (agg.groupingExpressions.size != 2) return None
      val g0 = pos(agg.groupingExpressions(0), l, r)
      val g1 = pos(agg.groupingExpressions(1), l, r)
      if (g0.isEmpty || g1.isEmpty || g0.get._1 != g1.get._1 || g0.get._2 == g1.get._2) return None
      val xIdx = g0.get._1
      var kIdx = -1
      var mode = 0
      var col0Smaller = true
      for (c <- splitConjunctivePredicates(j.condition.get)) {
        c match {
          case EqualTo(a, b) if kIdx < 0 && pair(a, b, l, r).exists(_._1 != xIdx) => kIdx = pair(a, b, l, r).get._1
          case LessThan(a, b) if mode == 0 && pair(a, b, l, r).exists(_._1 == xIdx) =>
            mode = 1; col0Smaller = pair(a, b, l, r).get._2 == 0
          case LessThanOrEqual(a, b) if mode == 0 && pair(a, b, l, r).exists(_._1 == xIdx) =>
            mode = 2; col0Smaller = pair(a, b, l, r).get._2 == 0
          case GreaterThan(a, b) if mode == 0 && pair(a, b, l, r).exists(_._1 == xIdx) =>
            mode = 1; col0Smaller = pair(a, b, l, r).get._2 == 1
          case GreaterThanOrEqual(a, b) if mode == 0 && pair(a, b, l, r).exists(_._1 == xIdx) =>
            mode = 2; col0Smaller = pair(a, b, l, r).get._2 == 1
          case _ => return None
        }
      }
      if (kIdx < 0) return None
      val xAttr = l.output(xIdx)
      if (!isKey(xAttr.dataType) || !isKey(l.output(kIdx).dataType)) return None
      if (xAttr.nullable && !l.constraints.exists {
            case IsNotNull(a: Attribute) => a.exprId == xAttr.exprId
            case _ => false
          }) return None
      var vIdx = -1
      var aggType: DataType = LongType
      val kinds = ArrayBuffer[Int]()
      for (e <- agg.aggregateExpressions) {
        val inner = e match { case Alias(c, _) => c; case other => other }
        pos(inner, l, r) match {
          case Some((i, s)) if i == xIdx => kinds += s
          case Some(_) => return None
          case None =>
            inner match {
              case AggregateExpression(f, Complete, false, None, _) if !kinds.contains(2) =>
                f match {
                  case c: Count if c.children.size == 1 && c.children.head.foldable &&
                      c.children.head.eval() != null => aggType = LongType
                  case s: Sum =>
                    s.child match {
                      case m: Multiply =>
                        pair(m.left, m.right, l, r) match {
                          case Some((i, _)) if i != kIdx =>
                            val vt = l.output(i).dataType
                            if (!(isInt(vt) || vt == DoubleType || vt == FloatType)) return None
                            if (l.output(i).nullable && !l.constraints.exists {
                                  case IsNotNull(a: Attribute) => a.exprId == l.output(i).exprId
                                  case _ => false
                                }) return None
                            vIdx = i
                            aggType = s.dataType
                          case _ => return None
                        }
                      case _ => return None
                    }
                  case _ => return None
                }
                kinds += 2
              case _ => return None
            }
        }
      }
      if (aggType != LongType && aggType != DoubleType) return None
      val spec = TesseraSpec(kinds.toSeq, kIdx, xIdx, vIdx, l.output(kIdx).dataType, xAttr.dataType,
        if (vIdx >= 0) l.output(vIdx).dataType else NullType, aggType, mode, col0Smaller)
      Some((spec, l))
    case _ => None
  }
}

case class TesseraPart(n: Int, k: Array[Long], kBytes: Array[Byte], kOffs: Array[Int], x: Array[Long],
                       xBytes: Array[Byte], xOffs: Array[Int], v: Array[Double])

case class TesseraCrossSpec(kinds: Seq[Int], kx: Int, xIdx: Int, vIdx: Int, ky: Int, yIdx: Int, wIdx: Int,
                            kxType: DataType, kyType: DataType, xType: DataType, yType: DataType, vType: DataType,
                            wType: DataType, aggType: DataType)

object TesseraCross extends PredicateHelper {
  private def isInt(t: DataType): Boolean = t == IntegerType || t == LongType || t == ShortType
  private def isKey(t: DataType): Boolean = isInt(t) || t == StringType
  private def notNull(p: LogicalPlan, a: Attribute): Boolean = !a.nullable || p.constraints.exists {
    case IsNotNull(b: Attribute) => b.exprId == a.exprId
    case _ => false
  }
  private def throughProjects(p: LogicalPlan): LogicalPlan = p match {
    case Project(list, c) if list.forall(_.isInstanceOf[AttributeReference]) => throughProjects(c)
    case other => other
  }

  def matchCross(agg: Aggregate): Option[(TesseraCrossSpec, LogicalPlan, LogicalPlan)] = throughProjects(agg.child) match {
    case j: Join if j.joinType == Inner && j.condition.isDefined && agg.groupingExpressions.size == 2 =>
      val (l, r) = (j.left, j.right)
      def pos(e: Expression): Option[(Int, Int)] = e match {
        case a: AttributeReference =>
          val i = l.output.indexWhere(_.exprId == a.exprId)
          val k = r.output.indexWhere(_.exprId == a.exprId)
          if (i >= 0) Some((i, 0)) else if (k >= 0) Some((k, 1)) else None
        case _ => None
      }
      val g0 = pos(agg.groupingExpressions(0))
      val g1 = pos(agg.groupingExpressions(1))
      if (g0.isEmpty || g1.isEmpty || g0.get._2 == g1.get._2) return None
      if (TesseraStrategy.sameInput(l, r) && g0.get._1 == g1.get._1) return None
      val sx = g0.get._2
      val (xp, yp) = if (sx == 0) (l, r) else (r, l)
      val conds = splitConjunctivePredicates(j.condition.get)
      if (conds.size != 1) return None
      val (kx, ky) = conds.head match {
        case EqualTo(a, b) =>
          (pos(a), pos(b)) match {
            case (Some((i, s)), Some((k, t))) if s != t => if (s == sx) (i, k) else (k, i)
            case _ => return None
          }
        case _ => return None
      }
      val kxType = xp.output(kx).dataType
      val kyType = yp.output(ky).dataType
      if (!(isInt(kxType) && isInt(kyType)) && !(kxType == StringType && kyType == StringType)) return None
      val xIdx = g0.get._1
      val yIdx = g1.get._1
      val xa = xp.output(xIdx)
      val ya = yp.output(yIdx)
      if (!isKey(xa.dataType) || !isKey(ya.dataType) || !notNull(xp, xa) || !notNull(yp, ya)) return None
      var vIdx = -1
      var wIdx = -1
      var aggType: DataType = LongType
      val kinds = ArrayBuffer[Int]()
      for (e <- agg.aggregateExpressions) {
        val inner = e match { case Alias(c, _) => c; case other => other }
        pos(inner) match {
          case Some((i, s)) if s == sx && i == xIdx => kinds += 0
          case Some((i, s)) if s != sx && i == yIdx => kinds += 1
          case Some(_) => return None
          case None =>
            inner match {
              case AggregateExpression(f, Complete, false, None, _) if !kinds.contains(2) =>
                f match {
                  case c: Count if c.children.size == 1 && c.children.head.foldable && c.children.head.eval() != null =>
                    aggType = LongType
                  case s: Sum =>
                    s.child match {
                      case m: Multiply =>
                        (pos(m.left), pos(m.right)) match {
                          case (Some((i, a)), Some((k, b))) if a != b =>
                            val (vi, wi) = if (a == sx) (i, k) else (k, i)
                            val vt = xp.output(vi).dataType
                            val wt = yp.output(wi).dataType
                            def num(t: DataType) = isInt(t) || t == DoubleType || t == FloatType
                            if (!num(vt) || !num(wt) || !notNull(xp, xp.output(vi)) || !notNull(yp, yp.output(wi))) return None
                            vIdx = vi
                            wIdx = wi
                            aggType = s.dataType
                          case _ => return None
                        }
                      case _ => return None
                    }
                  case _ => return None
                }
                kinds += 2
              case _ => return None
            }
        }
      }
      if (aggType != LongType && aggType != DoubleType) return None
      Some((TesseraCrossSpec(kinds.toSeq, kx, xIdx, vIdx, ky, yIdx, wIdx, kxType, kyType, xa.dataType, ya.dataType,
        if (vIdx >= 0) xp.output(vIdx).dataType else NullType, if (wIdx >= 0) yp.output(wIdx).dataType else NullType,
        aggType), xp, yp))
    case _ => None
  }

  def collect(plan: SparkPlan, kIdx: Int, xIdx: Int, vIdx: Int, kType: DataType, xType: DataType,
              vType: DataType): Array[TesseraPart] = {
    val kStr = kType == StringType
    val xStr = xType == StringType
    plan.execute().mapPartitions { it =>
      val k = new ArrayBuffer[Long](); val x = new ArrayBuffer[Long](); val v = new ArrayBuffer[Double]()
      val kb = new ByteArrayOutputStream(); val xb = new ByteArrayOutputStream()
      val ko = ArrayBuffer[Int](0); val xo = ArrayBuffer[Int](0)
      def int(r: InternalRow, i: Int, t: DataType): Long = t match {
        case LongType => r.getLong(i); case ShortType => r.getShort(i).toLong; case _ => r.getInt(i).toLong
      }
      def str(r: InternalRow, i: Int, b: ByteArrayOutputStream, o: ArrayBuffer[Int]): Unit = {
        val u = r.getUTF8String(i).getBytes
        b.write(u, 0, u.length)
        o += b.size()
      }
      var n = 0
      it.foreach { r =>
        if (!r.isNullAt(kIdx)) {
          if (kStr) str(r, kIdx, kb, ko) else k += int(r, kIdx, kType)
          if (xStr) str(r, xIdx, xb, xo) else x += int(r, xIdx, xType)
          if (vIdx >= 0) v += (vType match {
            case DoubleType => r.getDouble(vIdx); case FloatType => r.getFloat(vIdx).toDouble
            case t => int(r, vIdx, t).toDouble
          })
          n += 1
        }
      }
      Iterator(TesseraPart(n, if (kStr) null else k.toArray, if (kStr) kb.toByteArray else null,
        if (kStr) ko.toArray else null, if (xStr) null else x.toArray, if (xStr) xb.toByteArray else null,
        if (xStr) xo.toArray else null, if (vIdx >= 0) v.toArray else null))
    }.collect()
  }
}

case class TesseraCrossExec(output: Seq[Attribute], spec: TesseraCrossSpec, left: SparkPlan, right: SparkPlan)
    extends BinaryExecNode {

  override protected def withNewChildrenInternal(newLeft: SparkPlan, newRight: SparkPlan): SparkPlan =
    copy(left = newLeft, right = newRight)

  override def simpleString(maxFields: Int): String =
    s"TesseraExec(cross product, agg=${if (spec.vIdx >= 0) "SUM(a.v * b.w)" else "COUNT(*)"})"

  override protected def doExecute(): RDD[InternalRow] = {
    val s = spec
    val kStr = s.kxType == StringType
    val xStr = s.xType == StringType
    val yStr = s.yType == StringType
    val hasV = s.vIdx >= 0
    val xs = TesseraCross.collect(left, s.kx, s.xIdx, s.vIdx, s.kxType, s.xType, s.vType)
    val ys = TesseraCross.collect(right, s.ky, s.yIdx, s.wIdx, s.kyType, s.yType, s.wType)
    val c = SQLConf.get
    val threads = c.getConfString("spark.tessera.threads", "0").toInt
    val verbose = c.getConfString("spark.tessera.verbose", "false").toBoolean
    val h = TesseraNative.createCross(hasV)
    val dk = if (kStr) TesseraNative.dictNew() else 0L
    val dx = if (xStr) TesseraNative.dictNew() else 0L
    val dy = if (yStr) TesseraNative.dictNew() else 0L
    try {
      def codes(d: Long, b: Array[Byte], o: Array[Int], n: Int): Array[Long] = {
        val out = new Array[Long](n)
        if (TesseraNative.dictCode(d, b, o, n, out) != 0) throw new OutOfMemoryError("tessera: dictionary")
        out
      }
      for (p <- xs if p.n > 0) {
        if (TesseraNative.add(h, if (kStr) codes(dk, p.kBytes, p.kOffs, p.n) else p.k,
          if (xStr) codes(dx, p.xBytes, p.xOffs, p.n) else p.x, p.v, p.n) != 0) throw new OutOfMemoryError("tessera: add")
      }
      for (p <- ys if p.n > 0) {
        if (TesseraNative.addY(h, if (kStr) codes(dk, p.kBytes, p.kOffs, p.n) else p.k,
          if (yStr) codes(dy, p.xBytes, p.xOffs, p.n) else p.x, p.v, p.n) != 0) throw new OutOfMemoryError("tessera: add")
      }
      if (xStr) TesseraNative.setXDict(h, dx)
      if (yStr) TesseraNative.setYDict(h, dy)
      val runner = Thread.currentThread()
      @volatile var running = true
      val watcher = new Thread(() => {
        while (running) {
          if (runner.isInterrupted) TesseraNative.cancel(h)
          try Thread.sleep(10) catch { case _: InterruptedException => }
        }
      })
      watcher.setDaemon(true)
      watcher.start()
      val err = try TesseraNative.run(h, 0, true, 0L, -1L, threads, -1L, verbose)
                finally { running = false; watcher.interrupt() }
      if (err != null) {
        if (runner.isInterrupted) throw new InterruptedException(err)
        throw new IllegalStateException(err)
      }
      val general = TesseraNative.general(h)
      val xDict = if (xStr) sparkContext.broadcast((TesseraNative.xStringBytes(h), TesseraNative.xStringOffsets(h))) else null
      val yDict = if (yStr) sparkContext.broadcast((TesseraNative.yStringBytes(h), TesseraNative.yStringOffsets(h))) else null
      val chunks = ArrayBuffer[(Array[Long], Array[Long], Array[Long], Array[Double], Int)]()
      var done = false
      while (!done) {
        val a = new Array[Long](1 << 20); val b = new Array[Long](1 << 20)
        val vi = new Array[Long](1 << 20); val vd = new Array[Double](1 << 20)
        val n = TesseraNative.next(h, a, b, vi, vd)
        if (n > 0) chunks += ((a, b, vi, vd, n))
        done = n < a.length
      }
      val types = output.map(_.dataType).toArray
      sparkContext.parallelize(chunks.toSeq, math.max(1, chunks.size)).mapPartitions { it =>
        val proj = UnsafeProjection.create(types)
        val row = new GenericInternalRow(types.length)
        val (xb, xo) = if (xStr) xDict.value else (null, null)
        val (yb, yo) = if (yStr) yDict.value else (null, null)
        def key(v: Long, str: Boolean, b: Array[Byte], o: Array[Long], t: DataType): Any =
          if (str) UTF8String.fromBytes(b, o(v.toInt).toInt, (o(v.toInt + 1) - o(v.toInt)).toInt)
          else if (t == LongType) v else if (t == ShortType) v.toShort else v.toInt
        it.flatMap { case (a, b, vi, vd, n) =>
          (0 until n).iterator.map { q =>
            for (col <- types.indices) {
              row.update(col, s.kinds(col) match {
                case 0 => key(a(q), xStr, xb, xo, s.xType)
                case 1 => key(b(q), yStr, yb, yo, s.yType)
                case _ => if (s.aggType == DoubleType) (if (general) vd(q) else vi(q).toDouble)
                          else (if (general) math.round(vd(q)) else vi(q))
              })
            }
            proj(row)
          }
        }
      }
    } finally {
      TesseraNative.free(h)
      if (dk != 0L) TesseraNative.dictFree(dk)
      if (dx != 0L) TesseraNative.dictFree(dx)
      if (dy != 0L) TesseraNative.dictFree(dy)
    }
  }
}

case class TesseraExec(output: Seq[Attribute], spec: TesseraSpec, child: SparkPlan) extends UnaryExecNode {

  override protected def withNewChildInternal(newChild: SparkPlan): SparkPlan = copy(child = newChild)

  override def simpleString(maxFields: Int): String =
    s"TesseraExec(mode=${spec.mode}, agg=${if (spec.vIdx >= 0) "SUM(a.v * b.v)" else "COUNT(*)"}" +
      (if (spec.simMeasure > 0) s", similarity=${spec.simMeasure} >= ${spec.minSim})" else ")")

  override protected def doExecute(): RDD[InternalRow] = {
    val s = spec
    val kStr = s.kType == StringType
    val xStr = s.xType == StringType
    val parts = child.execute().mapPartitions { it =>
      val k = new ArrayBuffer[Long](); val x = new ArrayBuffer[Long](); val v = new ArrayBuffer[Double]()
      val kb = new ByteArrayOutputStream(); val xb = new ByteArrayOutputStream()
      val ko = ArrayBuffer[Int](0); val xo = ArrayBuffer[Int](0)
      def int(r: InternalRow, i: Int, t: DataType): Long = t match {
        case LongType => r.getLong(i); case ShortType => r.getShort(i).toLong; case _ => r.getInt(i).toLong
      }
      def str(r: InternalRow, i: Int, b: ByteArrayOutputStream, o: ArrayBuffer[Int]): Unit = {
        val u = r.getUTF8String(i).getBytes
        b.write(u, 0, u.length)
        o += b.size()
      }
      var n = 0
      it.foreach { r =>
        if (!r.isNullAt(s.kIdx)) {
          if (r.isNullAt(s.xIdx) || (s.vIdx >= 0 && r.isNullAt(s.vIdx)))
            throw new IllegalArgumentException("tessera: NULL group keys or values are not supported")
          if (kStr) str(r, s.kIdx, kb, ko) else k += int(r, s.kIdx, s.kType)
          if (xStr) str(r, s.xIdx, xb, xo) else x += int(r, s.xIdx, s.xType)
          if (s.vIdx >= 0) v += (s.vType match {
            case DoubleType => r.getDouble(s.vIdx); case FloatType => r.getFloat(s.vIdx).toDouble
            case t => int(r, s.vIdx, t).toDouble
          })
          n += 1
        }
      }
      Iterator(TesseraPart(n, if (kStr) null else k.toArray, if (kStr) kb.toByteArray else null,
        if (kStr) ko.toArray else null, if (xStr) null else x.toArray, if (xStr) xb.toByteArray else null,
        if (xStr) xo.toArray else null, if (s.vIdx >= 0) v.toArray else null))
    }.collect()
    val c = SQLConf.get
    val threads = c.getConfString("spark.tessera.threads", "0").toInt
    val verbose = c.getConfString("spark.tessera.verbose", "false").toBoolean
    val h = TesseraNative.create(s.vIdx >= 0)
    val dk = if (kStr) TesseraNative.dictNew() else 0L
    val dx = if (xStr) TesseraNative.dictNew() else 0L
    try {
      def codes(d: Long, b: Array[Byte], o: Array[Int], n: Int): Array[Long] = {
        val out = new Array[Long](n)
        if (TesseraNative.dictCode(d, b, o, n, out) != 0) throw new OutOfMemoryError("tessera: dictionary")
        out
      }
      for (p <- parts if p.n > 0) {
        val k = if (kStr) codes(dk, p.kBytes, p.kOffs, p.n) else p.k
        val x = if (xStr) codes(dx, p.xBytes, p.xOffs, p.n) else p.x
        if (TesseraNative.add(h, k, x, p.v, p.n) != 0) throw new OutOfMemoryError("tessera: add")
      }
      if (xStr) TesseraNative.setXDict(h, dx)
      if (s.simMeasure > 0) TesseraNative.setSimilarity(h, s.simMeasure, s.minSim)
      val runner = Thread.currentThread()
      @volatile var running = true
      val watcher = new Thread(() => {
        while (running) {
          if (runner.isInterrupted) TesseraNative.cancel(h)
          try Thread.sleep(10) catch { case _: InterruptedException => }
        }
      })
      watcher.setDaemon(true)
      watcher.start()
      val err = try TesseraNative.run(h, s.mode, s.col0Smaller, 0L, -1L, threads, -1L, verbose)
                finally { running = false; watcher.interrupt() }
      if (err != null) {
        if (runner.isInterrupted) throw new InterruptedException(err)
        throw new IllegalStateException(err)
      }
      val general = TesseraNative.general(h)
      val xDict = if (xStr) sparkContext.broadcast((TesseraNative.xStringBytes(h), TesseraNative.xStringOffsets(h)))
                  else null
      val chunks = ArrayBuffer[(Array[Long], Array[Long], Array[Long], Array[Double], Int)]()
      var done = false
      while (!done) {
        val a = new Array[Long](1 << 20); val b = new Array[Long](1 << 20)
        val vi = new Array[Long](1 << 20); val vd = new Array[Double](1 << 20)
        val n = TesseraNative.next(h, a, b, vi, vd)
        if (n > 0) chunks += ((a, b, vi, vd, n))
        done = n < a.length
      }
      val types = output.map(_.dataType).toArray
      sparkContext.parallelize(chunks.toSeq, math.max(1, chunks.size)).mapPartitions { it =>
        val proj = UnsafeProjection.create(types)
        val row = new GenericInternalRow(types.length)
        val (db, dof) = if (xStr) xDict.value else (null, null)
        def key(x: Long): Any =
          if (xStr) UTF8String.fromBytes(db, dof(x.toInt).toInt, (dof(x.toInt + 1) - dof(x.toInt)).toInt)
          else if (s.xType == LongType) x else if (s.xType == ShortType) x.toShort else x.toInt
        it.flatMap { case (a, b, vi, vd, n) =>
          (0 until n).iterator.map { q =>
            for (col <- types.indices) {
              row.update(col, s.kinds(col) match {
                case 0 => key(a(q))
                case 1 => key(b(q))
                case _ => if (s.aggType == DoubleType) (if (general) vd(q) else vi(q).toDouble)
                          else (if (general) math.round(vd(q)) else vi(q))
              })
            }
            proj(row)
          }
        }
      }
    } finally {
      TesseraNative.free(h)
      if (dk != 0L) TesseraNative.dictFree(dk)
      if (dx != 0L) TesseraNative.dictFree(dx)
    }
  }
}
