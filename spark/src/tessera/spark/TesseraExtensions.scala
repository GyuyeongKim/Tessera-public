package tessera.spark

import java.io.ByteArrayOutputStream

import scala.collection.mutable.ArrayBuffer

import org.apache.spark.rdd.RDD
import org.apache.spark.sql.SparkSessionExtensions
import org.apache.spark.sql.catalyst.InternalRow
import org.apache.spark.sql.catalyst.expressions._
import org.apache.spark.sql.catalyst.expressions.aggregate.{AggregateExpression, Average, Complete, Count, Sum}
import org.apache.spark.sql.catalyst.plans.Inner
import org.apache.spark.sql.catalyst.plans.logical.{Aggregate, Filter, Join, Limit, LimitAndOffset, LogicalPlan,
  OffsetAndLimit, Project, ReturnAnswer, Sort}
import org.apache.spark.sql.execution.{BinaryExecNode, FilterExec, ProjectExec, SparkPlan, SparkStrategy,
  TakeOrderedAndProjectExec, UnaryExecNode}
import org.apache.spark.sql.execution.adaptive.LogicalQueryStage
import org.apache.spark.sql.internal.SQLConf
import org.apache.spark.sql.types._
import org.apache.spark.unsafe.types.UTF8String

object TesseraConf {
  private val LocalN = "local\\[(\\*|\\d+)(?:,\\d+)?\\]".r
  def threads(c: SQLConf): Int = {
    val t = c.getConfString("spark.tessera.threads", "0").toInt
    if (t > 0) t
    else {
      val sc = org.apache.spark.SparkContext.getOrCreate()
      sc.master match {
        case "local" => 1
        case LocalN(n) => if (n == "*") Runtime.getRuntime.availableProcessors else n.toInt
        case _ => sc.getConf.getInt("spark.driver.cores", 1)
      }
    }
  }
}

object TesseraLimit {
  private def side(p: LogicalPlan, k: Attribute): Option[(Double, Double)] = {
    val st = p.stats
    for (n <- st.rowCount if n > 0; cs <- st.attributeStats.get(k); d <- cs.distinctCount if d > 0)
      yield (n.toDouble, d.toDouble)
  }

  def expansion(a: LogicalPlan, ka: Attribute, b: Option[(LogicalPlan, Attribute)]): Double =
    (side(a, ka), b.map { case (p, k) => side(p, k) }) match {
      case (Some((na, da)), None) => na / da
      case (Some((na, da)), Some(Some((nb, db)))) => na * nb / (math.max(da, db) * (na + nb))
      case _ => -1
    }

  def run(a: LogicalPlan, ka: Attribute, b: Option[(LogicalPlan, Attribute)]): Boolean = {
    val c = SQLConf.get
    val theta = c.getConfString(if (b.isDefined) "spark.tessera.minExpansionCross" else "spark.tessera.minExpansion",
      "0").toDouble
    val verbose = c.getConfString("spark.tessera.verbose", "false").toBoolean
    if (theta <= 0 && !verbose) return true
    val e = expansion(a, ka, b)
    val ok = theta <= 0 || e < 0 || e >= theta
    if (verbose) {
      System.err.println(f"tessera: $e%.3g joined rows per input row (estimate), limit $theta%.3g: " +
        (if (ok) "Tessera" else "Spark's plan"))
    }
    ok
  }
}

class TesseraExtensions extends (SparkSessionExtensions => Unit) {
  override def apply(e: SparkSessionExtensions): Unit = e.injectPlannerStrategy(_ => TesseraStrategy)
}

case class TesseraSpec(kinds: Seq[Int], kIdx: Int, xIdx: Int, vIdx: Int, kType: DataType, xType: DataType,
                       vType: DataType, aggType: DataType, mode: Int, col0Smaller: Boolean, simMeasure: Int = 0,
                       minSim: Double = 0.0, vInt: Boolean = false, ansi: Boolean = true, distinctKeys: Boolean = false,
                       minValue: Long = 0L, topk: Long = -1L)

object TesseraStrategy extends SparkStrategy with PredicateHelper {
  private lazy val cpuOk = TesseraNative.supported()

  private def on: Boolean = cpuOk && SQLConf.get.getConfString("spark.tessera.enabled", "true").toBoolean

  override def apply(plan: LogicalPlan): Seq[SparkPlan] = plan match {
    case n: TesseraSimNode =>
      if (!cpuOk) throw new UnsupportedOperationException("tessera: this CPU lacks x86-64-v3 (AVX2, BMI2, FMA), which Tessera needs")
      Seq(TesseraExec(n.output, n.spec, planLater(n.child)))
    case _ if !on => Nil
    case ReturnAnswer(root) => topK(root).toSeq
    case Filter(cond, a: Aggregate) => tesseraOf(a).map { case (_, build) => FilterExec(cond, build(Some(cond), -1L)) }.toSeq
    case a: Aggregate => tesseraOf(a).map { case (_, build) => build(None, -1L) }.toSeq
    case other => topK(other).toSeq
  }

  private def tesseraOf(a: Aggregate): Option[(Set[ExprId], (Option[Expression], Long) => SparkPlan)] = {
    def aggCols(agg: Seq[Boolean]) = agg.zip(a.output).collect { case (true, at) => at.exprId }.toSet
    matchAgg(a) match {
      case Some((spec, left)) =>
        if (!TesseraLimit.run(left, left.output(spec.kIdx), None)) None
        else {
          val cols = aggCols(spec.kinds.map(_ == 2))
          val prune = !(spec.vInt && !spec.ansi)
          Some((cols, (having: Option[Expression], topk: Long) =>
            TesseraExec(a.output, spec.copy(minValue = if (prune) minValue(having, cols) else 0L,
              topk = if (prune) topk else -1L), planLater(left))))
        }
      case None =>
        TesseraCross.matchCross(a).filter { case (spec, xp, yp, _, _, _) =>
          TesseraLimit.run(xp, xp.output(spec.kx.head), Some((yp, yp.output(spec.ky.head))))
        }.map { case (spec, xp, yp, proj, xb, yb) =>
          def input(p: LogicalPlan, base: LogicalPlan): SparkPlan = p match {
            case Project(list, _) if !(p eq base) => ProjectExec(list, planLater(base))
            case _ => planLater(base)
          }
          val cols = if (proj.isDefined) Set.empty[ExprId] else aggCols(spec.kinds.map(_._1 == 2))
          val prune = spec.two || !(spec.vInt && !spec.ansi)
          (cols, (having: Option[Expression], topk: Long) => {
            val exec = TesseraCrossExec(proj.map(_._1).getOrElse(a.output), spec.copy(
              minValue = if (prune) minValue(having, cols) else 0L,
              topk = if (prune && spec.filters.isEmpty && proj.isEmpty) topk else -1L), input(xp, xb), input(yp, yb))
            proj.map { case (_, list) => ProjectExec(list, exec) }.getOrElse(exec)
          })
        }
    }
  }

  private def minValue(cond: Option[Expression], cols: Set[ExprId]): Long = {
    def boundOf(l: Literal): Long = {
      val d = l.value match {
        case n: java.lang.Number => n.doubleValue
        case d: Decimal => d.toDouble
        case _ => 0.0
      }
      if (d >= 1 && d < 9007199254740992.0) math.floor(d).toLong else 0L
    }
    cond.toSeq.flatMap(splitConjunctivePredicates).map {
      case GreaterThanOrEqual(a: Attribute, l: Literal) if cols(a.exprId) => boundOf(l)
      case GreaterThan(a: Attribute, l: Literal) if cols(a.exprId) => boundOf(l)
      case LessThanOrEqual(l: Literal, a: Attribute) if cols(a.exprId) => boundOf(l)
      case LessThan(l: Literal, a: Attribute) if cols(a.exprId) => boundOf(l)
      case _ => 0L
    }.foldLeft(0L)(math.max)
  }

  private def lowerBoundsOn(cond: Option[Expression], at: ExprId): Boolean =
    cond.toSeq.flatMap(splitConjunctivePredicates).forall {
      case GreaterThanOrEqual(a: Attribute, _: Literal) => a.exprId == at
      case GreaterThan(a: Attribute, _: Literal) => a.exprId == at
      case LessThanOrEqual(_: Literal, a: Attribute) => a.exprId == at
      case LessThan(_: Literal, a: Attribute) => a.exprId == at
      case _ => false
    }

  private def topK(p: LogicalPlan): Option[SparkPlan] = {
    val thr = SQLConf.get.topKSortFallbackThreshold
    def take(limit: Int, offset: Int, order: Seq[SortOrder], projectList: Option[Seq[NamedExpression]],
             child: LogicalPlan): Option[SparkPlan] = {
      val (having, a) = child match {
        case Filter(c, a: Aggregate) => (Some(c), a)
        case a: Aggregate => (None, a)
        case _ => return None
      }
      tesseraOf(a).flatMap { case (cols, build) =>
        val ordered = order.headOption.filter(_.direction == Descending).map(_.child).collect {
          case at: Attribute if cols(at.exprId) => at.exprId
        }
        if (ordered.isEmpty) None
        else {
          val t = build(having, if (lowerBoundsOn(having, ordered.get)) limit.toLong else -1L)
          Some(TakeOrderedAndProjectExec(limit, order, projectList.getOrElse(child.output),
            having.map(FilterExec(_, t)).getOrElse(t), offset))
        }
      }
    }
    p match {
      case LimitAndOffset(limit, offset, Sort(order, true, child, _)) if limit < thr =>
        take(limit, offset, order, None, child)
      case LimitAndOffset(limit, offset, Project(pl, Sort(order, true, child, _))) if limit < thr =>
        take(limit, offset, order, Some(pl), child)
      case OffsetAndLimit(offset, limit, Sort(order, true, child, _)) if offset + limit < thr =>
        take(offset + limit, offset, order, None, child)
      case OffsetAndLimit(offset, limit, Project(pl, Sort(order, true, child, _))) if offset + limit < thr =>
        take(offset + limit, offset, order, Some(pl), child)
      case Limit(IntegerLiteral(limit), Sort(order, true, child, _)) if limit < thr =>
        take(limit, 0, order, None, child)
      case Limit(IntegerLiteral(limit), Project(pl, Sort(order, true, child, _))) if limit < thr =>
        take(limit, 0, order, Some(pl), child)
      case _ => None
    }
  }

  private def isInt(t: DataType): Boolean = t == IntegerType || t == LongType || t == ShortType

  private def unstage(p: LogicalPlan): LogicalPlan = p match {
    case s: LogicalQueryStage => s.logicalPlan
    case other => other
  }
  def sameInput(l: LogicalPlan, r: LogicalPlan): Boolean = unstage(l).sameResult(unstage(r))

  private def isKey(t: DataType): Boolean = isInt(t) || t == StringType || TesseraCross.isTemporal(t)

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

  def through(p: LogicalPlan, names: Map[ExprId, Attribute] = Map.empty): (LogicalPlan, Map[ExprId, Attribute]) = p match {
    case Project(list, c) if list.forall {
          case _: AttributeReference => true
          case Alias(_: AttributeReference, _) => true
          case _ => false
        } =>
      val inner = list.collect { case a @ Alias(r: AttributeReference, _) => a.exprId -> r.toAttribute }.toMap
      through(c, names.map { case (k, v) => k -> inner.getOrElse(v.exprId, v) } ++ inner)
    case other => (other, names)
  }
  def unname(e: Expression, names: Map[ExprId, Attribute]): Expression =
    if (names.isEmpty) e else e.transform { case a: AttributeReference if names.contains(a.exprId) => names(a.exprId) }

  def countsAll(e: Expression, l: LogicalPlan, r: LogicalPlan): Boolean = e match {
    case c if c.foldable => c.eval() != null
    case a: AttributeReference =>
      Seq(l, r).exists(p => p.output.exists(o => o.exprId == a.exprId && (!o.nullable || p.constraints.exists {
        case IsNotNull(b: Attribute) => b.exprId == a.exprId
        case _ => false
      })))
    case _ => false
  }

  private def matchAgg(agg: Aggregate): Option[(TesseraSpec, LogicalPlan)] = through(agg.child) match {
    case (j: Join, names) if j.joinType == Inner && j.condition.isDefined && sameInput(j.left, j.right) =>
      val (l, r) = (j.left, j.right)
      if (agg.groupingExpressions.size != 2) return None
      val g0 = pos(unname(agg.groupingExpressions(0), names), l, r)
      val g1 = pos(unname(agg.groupingExpressions(1), names), l, r)
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
          case Not(EqualTo(a, b)) if mode == 0 && pair(a, b, l, r).exists(_._1 == xIdx) => mode = 3
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
      var vInt = false
      var ansi = true
      var distinct = false
      var first: Option[Expression] = None
      val kinds = ArrayBuffer[Int]()
      for (e <- agg.aggregateExpressions) {
        val inner = unname(e match { case Alias(c, _) => c; case other => other }, names)
        pos(inner, l, r) match {
          case Some((i, s)) if i == xIdx => kinds += s
          case Some(_) => return None
          case None =>
            inner match {
              case ae: AggregateExpression if first.exists(_.semanticEquals(ae)) => kinds += 2
              case AggregateExpression(c: Count, Complete, true, None, _) if first.isEmpty && c.children.size == 1 &&
                  pos(c.children.head, l, r).exists(_._1 == kIdx) =>
                distinct = true
                first = Some(inner)
                kinds += 2
              case AggregateExpression(f, Complete, false, None, _) if first.isEmpty =>
                f match {
                  case c: Count if c.children.size == 1 && countsAll(c.children.head, l, r) => aggType = LongType
                  case s: Sum =>
                    s.child match {
                      case m: Multiply =>
                        pair(m.left, m.right, l, r) match {
                          case Some((i, _)) if i != kIdx =>
                            val vt = l.output(i).dataType
                            if (!(isInt(vt) || vt == DoubleType)) return None
                            if (!TesseraEvalModes.ok(s, m, isInt(vt), vt == LongType, vt == ShortType)) return None
                            vInt = isInt(vt)
                            ansi = m.evalContext.evalMode == EvalMode.ANSI
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
                first = Some(inner)
                kinds += 2
              case _ => return None
            }
        }
      }
      if (aggType != LongType && aggType != DoubleType) return None
      val spec = TesseraSpec(kinds.toSeq, kIdx, xIdx, vIdx, l.output(kIdx).dataType, xAttr.dataType,
        if (vIdx >= 0) l.output(vIdx).dataType else NullType, aggType, mode, col0Smaller, vInt = vIdx >= 0 && vInt,
        ansi = ansi, distinctKeys = distinct)
      Some((spec, l))
    case _ => None
  }
}

case class TesseraPart(n: Int, k: Array[Long], kBytes: Array[Byte], kOffs: Array[Int], x: Array[Long],
                       xBytes: Array[Byte], xOffs: Array[Int], v: Array[Double], iv: Array[Long] = null)

object TesseraPairFilter {
  def cols(v: Long, dict: Boolean, b: Array[Byte], o: Array[Long], t: Seq[DataType], nulls: Seq[Boolean]): Array[Any] =
    if (!dict) Array[Any](v)
    else if (t.size == 1 && !nulls.contains(true))
      Array[Any](TesseraCross.valueOf(b, o(v.toInt).toInt, (o(v.toInt + 1) - o(v.toInt)).toInt, t.head))
    else TesseraTuple.decode(b, o(v.toInt).toInt, o(v.toInt + 1).toInt, t, nulls)
  def pass(a: Any, b: Any, op: Int): Boolean = if (a == null || b == null) op == 5 && (a == null) != (b == null) else {
    val c = (a, b) match {
      case (x: Long, y: Long) => java.lang.Long.compare(x, y)
      case (x: UTF8String, y: UTF8String) => x.binaryCompare(y)
      case (x, y) => if (x == y) 0 else 1
    }
    op match { case 0 => c < 0; case 1 => c <= 0; case 2 => c > 0; case 3 => c >= 0; case _ => c != 0 }
  }
}

final class TesseraPairFilter(h: Long, s: TesseraCrossSpec) {
  private lazy val xd = if (s.xDict) (TesseraNative.xStringBytes(h), TesseraNative.xStringOffsets(h)) else (null, null)
  private lazy val yd = if (s.yDict && !s.symmetric) (TesseraNative.yStringBytes(h), TesseraNative.yStringOffsets(h))
                        else xd
  def pass(a: Long, b: Long): Boolean = (0 until (if (s.symmetric) 2 else 1)).exists { o =>
    val xc = TesseraPairFilter.cols(if (o == 0) a else b, s.xDict, xd._1, xd._2, s.xTypes, s.xNull)
    val yc = TesseraPairFilter.cols(if (o == 0) b else a, s.yDict, yd._1, yd._2, s.yTypes,
      if (s.symmetric) s.xNull else s.yNull)
    s.filters.forall { case (xi, yi, op) => TesseraPairFilter.pass(xc(xi), yc(yi), op) }
  }
}

object TesseraEvalModes {
  def ok(s: Sum, m: Multiply, int: Boolean, long: Boolean, short: Boolean): Boolean = {
    val sm = s.evalContext.evalMode
    val mm = m.evalContext.evalMode
    if (sm == EvalMode.TRY || mm == EvalMode.TRY || short) false
    else if (!int) true
    else sm == mm && (mm == EvalMode.ANSI || long)
  }
  def okAvg(m: Multiply, int: Boolean, short: Boolean): Boolean = {
    val mm = m.evalContext.evalMode
    mm != EvalMode.TRY && !short && (!int || mm == EvalMode.ANSI)
  }
  def check(h: Long, vType: DataType, sum: Boolean = true): Unit = {
    val (bits, name) = vType match { case LongType => (64, "long"); case _ => (32, "integer") }
    val ov = TesseraNative.overflow(h, bits, if (sum) 64 else 0)
    if ((ov & 1) != 0) throw org.apache.spark.sql.tessera.TesseraErrors.arithmeticOverflow(s"$name overflow", "try_multiply")
    if ((ov & 2) != 0) throw org.apache.spark.sql.tessera.TesseraErrors.arithmeticOverflow("long overflow", "try_add")
  }
}

object TesseraRun {
  def handle(h: Long, what: String): Long =
    if (h == 0L) throw new IllegalStateException(s"tessera: out of memory ($what)") else h
  def check(alive: () => Boolean): Unit =
    if (!alive()) throw new InterruptedException("tessera: the statement was cancelled")
  def run(h: Long, alive: () => Boolean, go: => String): Unit = {
    val runner = Thread.currentThread()
    @volatile var running = true
    val watcher = new Thread(() => {
      while (running) {
        if (runner.isInterrupted || !alive()) TesseraNative.cancel(h)
        try Thread.sleep(10) catch { case _: InterruptedException => }
      }
    })
    watcher.setDaemon(true)
    watcher.start()
    val err = try go finally {
      running = false
      watcher.interrupt()
      var interrupted = false
      var joined = false
      while (!joined) {
        try { watcher.join(); joined = true } catch { case _: InterruptedException => interrupted = true }
      }
      if (interrupted) runner.interrupt()
    }
    if (err != null) {
      if (runner.isInterrupted || !alive() || TesseraNative.lastStatus() == -2) throw new InterruptedException(err)
      throw new IllegalStateException(err)
    }
  }
}

case class TesseraCrossSpec(kinds: Seq[(Int, Int)], kx: Seq[Int], ky: Seq[Int], kxTypes: Seq[DataType],
                            kyTypes: Seq[DataType], xIdx: Seq[Int], yIdx: Seq[Int], xTypes: Seq[DataType],
                            yTypes: Seq[DataType], vIdx: Int, wIdx: Int, vType: DataType, wType: DataType,
                            aggType: DataType, hasValue: Boolean = false, vInt: Boolean = false, ansi: Boolean = true,
                            distinctKeys: Boolean = false, minValue: Long = 0L, topk: Long = -1L,
                            filters: Seq[(Int, Int, Int)] = Nil, two: Boolean = false, sumType: DataType = NullType,
                            symmetric: Boolean = false, sumCheck: Boolean = true, dec: Boolean = false,
                            decScale: Int = 0, avgAnsi: Boolean = true, decArgPrec: Int = 0, xNull: Seq[Boolean] = Nil,
                            yNull: Seq[Boolean] = Nil) {
  private def single(t: Seq[DataType]) = t.size == 1 && TesseraCross.heldAsLong(t.head)
  def kStr: Boolean = !single(kxTypes)
  def xDict: Boolean = !single(xTypes) || xNull.contains(true)
  def yDict: Boolean = !single(yTypes) || yNull.contains(true)
}

object TesseraTuple {
  def encode(r: InternalRow, idx: Seq[Int], types: Seq[DataType], out: ByteArrayOutputStream,
             nulls: Seq[Boolean] = Nil): Unit = {
    val b = java.nio.ByteBuffer.allocate(8).order(java.nio.ByteOrder.LITTLE_ENDIAN)
    for (c <- idx.indices) {
      b.clear()
      if (nulls.nonEmpty && nulls(c)) out.write(if (r.isNullAt(idx(c))) 0 else 1)
      if (nulls.nonEmpty && nulls(c) && r.isNullAt(idx(c))) {
      } else if (types(c) == StringType || TesseraCross.isTextKey(types(c))) {
        val u = TesseraCross.bytesOf(r, idx(c), types(c))
        b.putInt(u.length)
        out.write(b.array, 0, 4)
        out.write(u, 0, u.length)
      } else {
        b.putLong(TesseraCross.int(r, idx(c), types(c)))
        out.write(b.array, 0, 8)
      }
    }
  }
  def decode(bytes: Array[Byte], from: Int, until: Int, types: Seq[DataType], nulls: Seq[Boolean] = Nil): Array[Any] = {
    val b = java.nio.ByteBuffer.wrap(bytes, from, until - from).order(java.nio.ByteOrder.LITTLE_ENDIAN)
    types.indices.map { c =>
      val t = types(c)
      if (nulls.nonEmpty && nulls(c) && b.get() == 0) null
      else if (t == StringType || TesseraCross.isTextKey(t)) {
        val n = b.getInt()
        val s = TesseraCross.valueOf(bytes, b.position(), n, t)
        b.position(b.position() + n)
        s
      } else b.getLong(): Any
    }.toArray
  }
}

object TesseraCross extends PredicateHelper {
  def isInt(t: DataType): Boolean = t == IntegerType || t == LongType || t == ShortType
  def isTemporal(t: DataType): Boolean = t == DateType || t == TimestampType || t == TimestampNTZType
  def heldAsLong(t: DataType): Boolean = isInt(t) || isTemporal(t)
  def isTextKey(t: DataType): Boolean = t.isInstanceOf[DecimalType] || t == BooleanType
  def bytesOf(r: InternalRow, i: Int, t: DataType): Array[Byte] = t match {
    case StringType => r.getUTF8String(i).getBytes
    case d: DecimalType => r.getDecimal(i, d.precision, d.scale).toJavaBigDecimal.toPlainString.getBytes("UTF-8")
    case _ => (if (r.getBoolean(i)) "t" else "f").getBytes("UTF-8")
  }
  def valueOf(b: Array[Byte], from: Int, n: Int, t: DataType): Any = t match {
    case StringType => UTF8String.fromBytes(b, from, n)
    case d: DecimalType => Decimal(new java.math.BigDecimal(new String(b, from, n, "UTF-8")), d.precision, d.scale)
    case _ => b(from) == 't'.toByte
  }
  def comparable(a: DataType, b: DataType): Boolean =
    (isInt(a) && isInt(b)) || (a == StringType && b == StringType) || (isTemporal(a) && a == b) || (isTextKey(a) && a == b)
  private def isKey(t: DataType): Boolean = isInt(t) || t == StringType || TesseraCross.isTemporal(t) || isTextKey(t)
  private def notNull(p: LogicalPlan, a: Attribute): Boolean = !a.nullable || p.constraints.exists {
    case IsNotNull(b: Attribute) => b.exprId == a.exprId
    case _ => false
  }
  def int(r: InternalRow, i: Int, t: DataType): Long = t match {
    case LongType | TimestampType | TimestampNTZType => r.getLong(i); case ShortType => r.getShort(i).toLong
    case d: DecimalType => r.getDecimal(i, d.precision, d.scale).toUnscaledLong
    case _ => r.getInt(i).toLong
  }

  def joinBelow(p: LogicalPlan): Option[(Join, Map[ExprId, Attribute], Seq[Alias])] = TesseraStrategy.through(p) match {
    case (j: Join, names) => Some((j, names, Nil))
    case (Project(list, c), names) if list.forall { case _: AttributeReference | _: Alias => true; case _ => false } =>
      TesseraStrategy.through(c) match {
        case (j: Join, inner) =>
          val renames = list.collect { case a @ Alias(r: AttributeReference, _) => a.exprId -> inner.getOrElse(r.exprId, r.toAttribute) }
          val computed = list.collect { case a @ Alias(e, _) if !e.isInstanceOf[AttributeReference] =>
            Alias(TesseraStrategy.unname(e, inner), a.name)(a.exprId) }
          val all = inner ++ renames
          Some((j, names.map { case (k, v) => k -> all.getOrElse(v.exprId, v) } ++ all, computed))
        case _ => None
      }
    case _ => None
  }

  def safeGroup(e: Expression): Boolean = {
    def lit(x: Expression): Option[Long] = x match {
      case Literal(v: java.lang.Number, t) if isInt(t) => Some(v.longValue)
      case _ => None
    }
    e.deterministic && (e match {
      case _: AttributeReference => true
      case l: Literal => l.value != null
      case m: Remainder => lit(m.right).exists(_ != 0) && safeGroup(m.left)
      case m: Pmod => lit(m.right).exists(_ != 0) && safeGroup(m.left)
      case m: IntegralDivide => lit(m.right).exists(v => v != 0 && v != -1) && safeGroup(m.left)
      case u: Upper => safeGroup(u.child)
      case u: Lower => safeGroup(u.child)
      case u: Substring => u.pos.foldable && u.len.foldable && safeGroup(u.str)
      case u: Concat => u.children.forall(c => c.dataType == StringType && safeGroup(c))
      case u: ConcatWs => u.children.forall(c => c.dataType == StringType && safeGroup(c))
      case c: Cast => ((c.dataType == StringType && (isInt(c.child.dataType) || c.child.dataType == DateType)) ||
                       (c.dataType == LongType && isInt(c.child.dataType)) ||
                       (isTemporal(c.dataType) && c.child.dataType == DateType)) && safeGroup(c.child)
      case t: TruncDate => t.format.foldable && safeGroup(t.date)
      case t: TruncTimestamp => t.format.foldable && safeGroup(t.timestamp)
      case f @ (_: Year | _: Month | _: DayOfMonth | _: Quarter | _: DayOfWeek | _: DayOfYear | _: Hour | _: Minute) =>
        f.children.forall(safeGroup)
      case _ => false
    })
  }

  def matchCross(agg: Aggregate): Option[(TesseraCrossSpec, LogicalPlan, LogicalPlan,
                                          Option[(Seq[Attribute], Seq[NamedExpression])], LogicalPlan, LogicalPlan)] =
    joinBelow(agg.child) match {
    case Some((j, names, computed)) if j.joinType == Inner && j.condition.isDefined && agg.groupingExpressions.size >= 2 =>
      val grouped = agg.groupingExpressions.flatMap(g => TesseraStrategy.unname(g, names).references.map(_.exprId)).toSet
      if (computed.exists(a => !grouped(a.exprId))) return None
      val onLeft = computed.filter(a => a.references.subsetOf(j.left.outputSet))
      val onRight = computed.filter(a => !onLeft.contains(a) && a.references.subsetOf(j.right.outputSet))
      if (onLeft.size + onRight.size != computed.size || !computed.forall(a => safeGroup(a.child))) return None
      val casts = agg.aggregateExpressions.flatMap(_.collect { case m: Multiply => Seq(m.left, m.right) }.flatten)
        .map(e => TesseraStrategy.unname(e, names)).collect {
          case c @ Cast(a: AttributeReference, DoubleType, _, _) if isInt(a.dataType) || a.dataType.isInstanceOf[DecimalType] => c
        }
      val castCols = casts.foldLeft(Seq.empty[(Expression, Alias)]) { (acc, c) =>
        if (acc.exists(_._1.semanticEquals(c))) acc else acc :+ (c -> Alias(c, "tessera_w")())
      }
      def castCol(e: Expression): Expression = castCols.find(_._1.semanticEquals(e)).map(_._2.toAttribute).getOrElse(e)
      val keyExprs = splitConjunctivePredicates(j.condition.get).collect { case EqualTo(a, b) => Seq(a, b) }.flatten
        .filter(e => !e.isInstanceOf[AttributeReference] && safeGroup(e) && e.references.nonEmpty &&
          (e.references.subsetOf(j.left.outputSet) || e.references.subsetOf(j.right.outputSet)))
      val keyCols = keyExprs.foldLeft(castCols) { (acc, e) =>
        if (acc.exists(_._1.semanticEquals(e))) acc else acc :+ (e -> Alias(e, "tessera_k")())
      }.drop(castCols.size)
      def keyCol(e: Expression): Expression = keyCols.find(_._1.semanticEquals(e)).map(_._2.toAttribute).getOrElse(e)
      val wLeft = (castCols ++ keyCols).map(_._2).filter(a => a.references.subsetOf(j.left.outputSet))
      val wRight = (castCols ++ keyCols).map(_._2).filter(a => !wLeft.contains(a) && a.references.subsetOf(j.right.outputSet))
      val l = if (onLeft.isEmpty && wLeft.isEmpty) j.left else Project(j.left.output ++ onLeft ++ wLeft, j.left)
      val r = if (onRight.isEmpty && wRight.isEmpty) j.right else Project(j.right.output ++ onRight ++ wRight, j.right)
      def pos(e: Expression): Option[(Int, Int)] = e match {
        case a: AttributeReference =>
          val i = l.output.indexWhere(_.exprId == a.exprId)
          val k = r.output.indexWhere(_.exprId == a.exprId)
          if (i >= 0) Some((i, 0)) else if (k >= 0) Some((k, 1)) else None
        case _ => None
      }
      val gs = agg.groupingExpressions.map(g => pos(TesseraStrategy.unname(g, names)))
      if (gs.exists(_.isEmpty)) return None
      val sx = gs.head.get._2
      val (xp, yp) = if (sx == 0) (l, r) else (r, l)
      val xIdx = gs.flatten.filter(_._2 == sx).map(_._1).distinct
      val yIdx = gs.flatten.filter(_._2 != sx).map(_._1).distinct
      if (yIdx.isEmpty) return None
      def sides(a: Expression, b: Expression): Option[(Int, Int, Boolean)] = (pos(a), pos(b)) match {
        case (Some((i, s)), Some((k, t))) if s != t => Some(if (s == sx) (i, k, true) else (k, i, false))
        case _ => None
      }
      val kx = ArrayBuffer[Int](); val ky = ArrayBuffer[Int]()
      val filters = ArrayBuffer[(Int, Int, Int)]()
      def filter(a: Expression, b: Expression, op: Int): Unit = sides(a, b) match {
        case Some((i, k, leftX)) if xIdx.contains(i) && yIdx.contains(k) =>
          val o = if (leftX) op else op match { case 0 => 2; case 1 => 3; case 2 => 0; case 3 => 1; case o => o }
          filters += ((xIdx.indexOf(i), yIdx.indexOf(k), o))
        case _ => throw new NoSuchElementException
      }
      try {
        for (c <- splitConjunctivePredicates(j.condition.get)) c match {
          case EqualTo(a, b) => sides(keyCol(a), keyCol(b)) match {
            case Some((i, k, _)) => kx += i; ky += k
            case None => return None
          }
          case LessThan(a, b) => filter(a, b, 0)
          case LessThanOrEqual(a, b) => filter(a, b, 1)
          case GreaterThan(a, b) => filter(a, b, 2)
          case GreaterThanOrEqual(a, b) => filter(a, b, 3)
          case Not(EqualTo(a, b)) => filter(a, b, 4)
          case Not(EqualNullSafe(a, b)) => filter(a, b, 5)
          case _ => return None
        }
      } catch { case _: NoSuchElementException => return None }
      if (kx.isEmpty) return None
      val kxTypes = kx.map(xp.output(_).dataType).toSeq
      val kyTypes = ky.map(yp.output(_).dataType).toSeq
      if (kxTypes.zip(kyTypes).exists { case (a, b) => !comparable(a, b) })
        return None
      val xTypes = xIdx.map(xp.output(_).dataType)
      val yTypes = yIdx.map(yp.output(_).dataType)
      if (!xIdx.forall(i => isKey(xp.output(i).dataType)) || !yIdx.forall(i => isKey(yp.output(i).dataType))) return None
      val xNull = xIdx.map(i => !notNull(xp, xp.output(i)))
      val yNull = yIdx.map(i => !notNull(yp, yp.output(i)))
      if (filters.exists { case (xi, yi, op) => !comparable(xTypes(xi), yTypes(yi)) || (isTextKey(xTypes(xi)) && op < 4) }) return None
      var vIdx = -1
      var wIdx = -1
      var hasValue = false
      var aggType: DataType = LongType
      var vInt = false
      var ansi = true
      var distinct = false
      var countAgg: Option[Expression] = None
      var sumAgg: Option[Expression] = None
      var avgAgg: Option[Expression] = None
      var sumType: DataType = NullType
      val kinds = ArrayBuffer[(Int, Int)]()
      def num(t: DataType) = isInt(t) || t == DoubleType || isDec(t)
      def isDec(t: DataType) = t match { case d: DecimalType => d.precision <= 18 && d.scale >= 0; case _ => false }
      def digits(t: DataType) = t match { case d: DecimalType => d.precision; case _ => 0 }
      def scaleOf(t: DataType) = t match { case d: DecimalType => d.scale; case _ => 0 }
      var dec = false
      var decScale = 0
      var decArgPrec = 0
      var avgAnsi = true
      var argOf: Option[Expression] = None
      def arg(child: Expression, prodOk: (Multiply, DataType, DataType) => Boolean, colOk: DataType => Boolean,
              colAnsi: Boolean): Boolean = argOf.forall(_.semanticEquals(child)) && {
        val ok = child match {
          case m: Multiply =>
            (pos(castCol(m.left)), pos(castCol(m.right))) match {
              case (Some((i, a)), Some((k, b))) if a != b =>
                val (vi, wi) = if (a == sx) (i, k) else (k, i)
                val vt = xp.output(vi).dataType
                val wt = yp.output(wi).dataType
                val decOk = !isDec(vt) && !isDec(wt) || isDec(vt) && isDec(wt) && digits(vt) + digits(wt) <= 18 &&
                  m.dataType == DecimalType(digits(vt) + digits(wt) + 1, scaleOf(vt) + scaleOf(wt))
                if (!num(vt) || !num(wt) || !notNull(xp, xp.output(vi)) || !notNull(yp, yp.output(wi)) ||
                    isInt(vt) != isInt(wt) || !decOk || !prodOk(m, vt, wt)) false
                else {
                  dec = isDec(vt)
                  decScale = scaleOf(vt) + scaleOf(wt)
                  decArgPrec = digits(vt) + digits(wt) + 1
                  vInt = isInt(vt) || dec
                  ansi = m.evalContext.evalMode == EvalMode.ANSI
                  vIdx = vi
                  wIdx = wi
                  true
                }
              case _ => false
            }
          case col: AttributeReference =>
            pos(col) match {
              case Some((i, side)) =>
                val p = if (side == sx) xp else yp
                val t = p.output(i).dataType
                if (!num(t) || !notNull(p, p.output(i)) || !colOk(t)) false
                else {
                  if (side == sx) vIdx = i else wIdx = i
                  dec = isDec(t)
                  decScale = scaleOf(t)
                  decArgPrec = digits(t)
                  vInt = isInt(t) || dec
                  ansi = colAnsi
                  true
                }
              case None => false
            }
          case _ => false
        }
        if (ok) {
          argOf = Some(child)
          hasValue = true
        }
        ok
      }
      val outs = agg.aggregateExpressions.map(e => TesseraStrategy.unname(e match { case Alias(c, _) => c; case o => o }, names))
      def partsOf(e: Expression): Seq[Expression] = e match {
        case a: AttributeReference => Seq(a)
        case ae: AggregateExpression => Seq(ae)
        case l: Literal => Nil
        case other => other.children.flatMap(partsOf)
      }
      val project = outs.exists(o => !o.isInstanceOf[AttributeReference] && !o.isInstanceOf[AggregateExpression])
      val parts = ArrayBuffer[Expression]()
      if (!project) parts ++= outs
      else for (o <- outs; p <- partsOf(o)) if (!parts.exists(_.semanticEquals(p))) parts += p
      if (project && outs.exists(o => !o.deterministic)) return None
      for (inner <- parts) {
        pos(inner) match {
          case Some((i, s)) if s == sx && xIdx.contains(i) => kinds += ((0, xIdx.indexOf(i)))
          case Some((i, s)) if s != sx && yIdx.contains(i) => kinds += ((1, yIdx.indexOf(i)))
          case Some(_) => return None
          case None =>
            inner match {
              case ae: AggregateExpression if countAgg.exists(_.semanticEquals(ae)) => kinds += ((2, 0))
              case ae: AggregateExpression if sumAgg.exists(_.semanticEquals(ae)) => kinds += ((3, 0))
              case AggregateExpression(c: Count, Complete, true, None, _) if countAgg.isEmpty && c.children.size == 1 &&
                  kx.size == 1 && pos(c.children.head).exists { case (i, s) => if (s == sx) i == kx(0) else i == ky(0) } =>
                distinct = true
                countAgg = Some(inner)
                kinds += ((2, 0))
              case AggregateExpression(c: Count, Complete, false, None, _) if countAgg.isEmpty && c.children.size == 1 &&
                  TesseraStrategy.countsAll(c.children.head, l, r) =>
                countAgg = Some(inner)
                kinds += ((2, 0))
              case ae: AggregateExpression if avgAgg.exists(_.semanticEquals(ae)) => kinds += ((4, 0))
              case AggregateExpression(s: Sum, Complete, false, None, _) if sumAgg.isEmpty =>
                val sm = s.evalContext.evalMode
                if (!arg(s.child, (m, vt, wt) => TesseraEvalModes.ok(s, m, isInt(vt), vt == LongType && wt == LongType,
                           vt == ShortType || wt == ShortType),
                         t => sm != EvalMode.TRY && t != ShortType && (!isInt(t) || t == LongType || sm == EvalMode.ANSI),
                         sm == EvalMode.ANSI)) return None
                aggType = s.dataType
                sumAgg = Some(inner)
                sumType = aggType
                kinds += ((3, 0))
              case AggregateExpression(a: Average, Complete, false, None, _) if avgAgg.isEmpty =>
                if (!arg(a.child, (m, vt, wt) => TesseraEvalModes.okAvg(m, isInt(vt), vt == ShortType || wt == ShortType),
                         _ => true, ansi) || (a.dataType != DoubleType && !a.dataType.isInstanceOf[DecimalType])) return None
                avgAnsi = a.evalMode == EvalMode.ANSI
                avgAgg = Some(inner)
                kinds += ((4, 0))
              case _ => return None
            }
        }
      }
      if (countAgg.isEmpty && sumAgg.isEmpty && avgAgg.isEmpty) return None
      if (avgAgg.isDefined && distinct) return None
      val two = Seq(countAgg, sumAgg, avgAgg).count(_.isDefined) >= 2 || avgAgg.isDefined
      if (!two) {
        for (i <- kinds.indices if kinds(i)._1 == 3) kinds(i) = (2, 0)
        if (countAgg.isDefined) aggType = LongType
      } else {
        aggType = LongType
        if (sumAgg.isEmpty) sumType = if (dec) DecimalType(38, decScale) else if (vInt) LongType else DoubleType
        if (sumType != LongType && sumType != DoubleType && !(dec && sumType.isInstanceOf[DecimalType])) return None
      }
      if (aggType != LongType && aggType != DoubleType && !(dec && aggType.isInstanceOf[DecimalType])) return None
      val symmetric = TesseraStrategy.sameInput(xp, yp) && kx == ky && xIdx == yIdx &&
        (if (hasValue) vIdx >= 0 && vIdx == wIdx else true)
      val proj = if (!project) None else {
        val internal: Seq[Attribute] = parts.toSeq.zipWithIndex.map {
          case (a: AttributeReference, _) => a
          case (p, i) => AttributeReference(s"tessera_part$i", p.dataType, p.nullable)()
        }
        def toInternal(e: Expression): Expression = e.transformDown {
          case x if parts.exists(_.semanticEquals(x)) => internal(parts.indexWhere(_.semanticEquals(x)))
        }
        val list: Seq[NamedExpression] = agg.aggregateExpressions.zip(outs).map {
          case (a: Alias, o) => Alias(toInternal(o), a.name)(exprId = a.exprId, qualifier = a.qualifier,
                                                              explicitMetadata = a.explicitMetadata)
          case (ne, o) => Alias(toInternal(o), ne.name)(exprId = ne.exprId, qualifier = ne.qualifier)
        }
        Some((internal, list))
      }
      Some((TesseraCrossSpec(kinds.toSeq, kx.toSeq, ky.toSeq, kxTypes, kyTypes, xIdx, yIdx, xTypes, yTypes, vIdx, wIdx,
        if (vIdx >= 0) xp.output(vIdx).dataType else if (wIdx >= 0) yp.output(wIdx).dataType else NullType,
        if (wIdx >= 0) yp.output(wIdx).dataType else if (vIdx >= 0) xp.output(vIdx).dataType else NullType,
        aggType, hasValue, vInt = hasValue && vInt, ansi = ansi, distinctKeys = distinct, filters = filters.toSeq,
        two = two, sumType = sumType, symmetric = symmetric, sumCheck = sumAgg.isDefined, dec = dec, decScale = decScale,
        avgAnsi = avgAnsi, decArgPrec = decArgPrec, xNull = xNull, yNull = yNull), xp, yp, proj,
        if (sx == 0) j.left else j.right, if (sx == 0) j.right else j.left))
    case _ => None
  }

  def collect(plan: SparkPlan, kIdx: Seq[Int], kTypes: Seq[DataType], gIdx: Seq[Int], gTypes: Seq[DataType], vIdx: Int,
              vType: DataType, hasValue: Boolean, vInt: Boolean, gNull: Seq[Boolean]): Array[TesseraPart] = {
    val kBytes = !(kIdx.size == 1 && heldAsLong(kTypes.head))
    val gBytes = !(gIdx.size == 1 && heldAsLong(gTypes.head)) || gNull.contains(true)
    val gTuple = gIdx.size > 1 || gNull.contains(true)
    plan.execute().mapPartitions { it =>
      val k = new ArrayBuffer[Long](); val x = new ArrayBuffer[Long](); val v = new ArrayBuffer[Double]()
      val iv = new ArrayBuffer[Long]()
      val kb = new ByteArrayOutputStream(); val xb = new ByteArrayOutputStream()
      val ko = ArrayBuffer[Int](0); val xo = ArrayBuffer[Int](0)
      def bytes(r: InternalRow, idx: Seq[Int], types: Seq[DataType], b: ByteArrayOutputStream, o: ArrayBuffer[Int],
                tuple: Boolean, nulls: Seq[Boolean]): Unit = {
        if (!tuple) {
          val u = bytesOf(r, idx.head, types.head)
          b.write(u, 0, u.length)
        } else TesseraTuple.encode(r, idx, types, b, nulls)
        o += b.size()
      }
      var n = 0
      it.foreach { r =>
        if (!kIdx.exists(r.isNullAt)) {
          if (gIdx.indices.exists(c => !gNull(c) && r.isNullAt(gIdx(c))) || (vIdx >= 0 && r.isNullAt(vIdx)))
            throw new IllegalArgumentException("tessera: NULL group keys or values are not supported")
          if (kBytes) bytes(r, kIdx, kTypes, kb, ko, kIdx.size > 1, Nil) else k += int(r, kIdx.head, kTypes.head)
          if (gBytes) bytes(r, gIdx, gTypes, xb, xo, gTuple, gNull) else x += int(r, gIdx.head, gTypes.head)
          if (hasValue && vInt) iv += (if (vIdx >= 0) int(r, vIdx, vType) else 1L)
          else if (hasValue) v += (if (vIdx < 0) 1.0 else vType match {
            case DoubleType => r.getDouble(vIdx); case FloatType => r.getFloat(vIdx).toDouble
            case t => int(r, vIdx, t).toDouble
          })
          n += 1
        }
      }
      Iterator(TesseraPart(n, if (kBytes) null else k.toArray, if (kBytes) kb.toByteArray else null,
        if (kBytes) ko.toArray else null, if (gBytes) null else x.toArray, if (gBytes) xb.toByteArray else null,
        if (gBytes) xo.toArray else null, if (hasValue && !vInt) v.toArray else null,
        if (hasValue && vInt) iv.toArray else null))
    }.collect()
  }
}

case class TesseraCrossExec(output: Seq[Attribute], spec: TesseraCrossSpec, left: SparkPlan, right: SparkPlan)
    extends BinaryExecNode {

  override protected def withNewChildrenInternal(newLeft: SparkPlan, newRight: SparkPlan): SparkPlan =
    copy(left = newLeft, right = newRight)

  override def simpleString(maxFields: Int): String =
    s"TesseraExec(cross product, agg=${if (spec.kinds.exists(_._1 == 4)) "AVG(a.v * b.w)" else if (spec.hasValue) "SUM(a.v * b.w)" else if (spec.distinctKeys) "COUNT(DISTINCT k)" else "COUNT(*)"}" +
      (if (spec.kx.size > 1) s", ${spec.kx.size}-column key" else "") +
      (if (spec.filters.nonEmpty) s", ${spec.filters.size} row filters" else "") +
      (if (spec.minValue > 0) s", prune below ${spec.minValue}" else "") + (if (spec.topk > 0) s", top ${spec.topk}" else "") + ")"

  override protected def doExecute(): RDD[InternalRow] = {
    val s = spec
    val kStr = s.kStr
    val xStr = s.xDict
    val yStr = s.yDict
    val xNull = if (s.xNull.isEmpty) s.xIdx.map(_ => false) else s.xNull
    val yNull = if (s.yNull.isEmpty) s.yIdx.map(_ => false) else s.yNull
    val xs = TesseraCross.collect(left, s.kx, s.kxTypes, s.xIdx, s.xTypes, s.vIdx, s.vType, s.hasValue, s.vInt, xNull)
    val ys = if (s.symmetric) Array.empty[TesseraPart]
             else TesseraCross.collect(right, s.ky, s.kyTypes, s.yIdx, s.yTypes, s.wIdx, s.wType, s.hasValue, s.vInt, yNull)
    val c = SQLConf.get
    val threads = TesseraConf.threads(c)
    val alive = org.apache.spark.sql.tessera.TesseraStatement.alive(sparkContext)
    val verbose = c.getConfString("spark.tessera.verbose", "false").toBoolean
    val h = TesseraRun.handle(if (s.symmetric) TesseraNative.create(s.hasValue) else TesseraNative.createCross(s.hasValue),
      "query handle")
    var dk, dx, dy = 0L
    try {
      if (kStr) dk = TesseraRun.handle(TesseraNative.dictNew(), "dictionary")
      if (xStr) dx = TesseraRun.handle(TesseraNative.dictNew(), "dictionary")
      if (yStr && !s.symmetric) dy = TesseraRun.handle(TesseraNative.dictNew(), "dictionary")
      def codes(d: Long, b: Array[Byte], o: Array[Int], n: Int): Array[Long] = {
        val out = new Array[Long](n)
        if (TesseraNative.dictCode(d, b, o, n, out) != 0) throw new IllegalStateException("tessera: out of memory (dictionary)")
        out
      }
      for (p <- xs if p.n > 0) {
        val k = if (kStr) codes(dk, p.kBytes, p.kOffs, p.n) else p.k
        val x = if (xStr) codes(dx, p.xBytes, p.xOffs, p.n) else p.x
        if ((if (s.vInt) TesseraNative.addI64(h, k, x, p.iv, p.n) else TesseraNative.add(h, k, x, p.v, p.n)) != 0)
          throw new IllegalStateException("tessera: out of memory (rows)")
      }
      for (p <- ys if p.n > 0) {
        val k = if (kStr) codes(dk, p.kBytes, p.kOffs, p.n) else p.k
        val y = if (yStr) codes(dy, p.xBytes, p.xOffs, p.n) else p.x
        if ((if (s.vInt) TesseraNative.addYI64(h, k, y, p.iv, p.n) else TesseraNative.addY(h, k, y, p.v, p.n)) != 0)
          throw new IllegalStateException("tessera: out of memory (rows)")
      }
      if (xStr) TesseraNative.setXDict(h, dx)
      if (yStr && !s.symmetric) TesseraNative.setYDict(h, dy)
      if (s.distinctKeys) TesseraNative.setDistinctKeys(h, true)
      if (s.two) TesseraNative.setTwo(h, true)
      val rowSums = s.vInt && s.ansi && !s.dec && s.filters.nonEmpty
      val pvt = if (s.vIdx >= 0) s.vType else s.wType
      if (rowSums) TesseraNative.setPairFilter(h, new TesseraPairFilter(h, s), if (pvt == LongType) 64 else 32)
      TesseraRun.run(h, alive, TesseraNative.run(h, 0, true, s.minValue, s.topk, threads, -1L, verbose))
      if (s.vInt && s.ansi && !s.dec) TesseraEvalModes.check(h, pvt, s.sumCheck && !rowSums)
      val general = TesseraNative.general(h)
      val xDict = if (xStr) sparkContext.broadcast((TesseraNative.xStringBytes(h), TesseraNative.xStringOffsets(h))) else null
      val yDict = if (yStr && s.symmetric) xDict
                  else if (yStr) sparkContext.broadcast((TesseraNative.yStringBytes(h), TesseraNative.yStringOffsets(h))) else null
      val chunks = ArrayBuffer[(Array[Long], Array[Long], Array[Long], Array[Double], Int, Array[Long], Array[Long])]()
      var done = false
      while (!done) {
        TesseraRun.check(alive)
        val a = new Array[Long](1 << 20); val b = new Array[Long](1 << 20)
        val cnt = if (s.two) new Array[Long](1 << 20) else null
        val vi = if (general && !s.two) null else new Array[Long](1 << 20)
        val vd = if (general || s.two) new Array[Double](1 << 20) else null
        val wideRows = s.dec || rowSums
        val hi = if (wideRows) new Array[Long](1 << 20) else null
        val n = if (wideRows && s.two) TesseraNative.next2Wide(h, a, b, cnt, hi, vi, vd)
                else if (wideRows) TesseraNative.next128(h, a, b, hi, vi)
                else if (s.two) TesseraNative.next2(h, a, b, cnt, vi, vd) else TesseraNative.next(h, a, b, vi, vd)
        if (n > 0) chunks += ((a, b, vi, vd, n, cnt, hi))
        done = n < a.length
      }
      val types = output.map(_.dataType).toArray
      sparkContext.parallelize(chunks.toSeq, math.max(1, chunks.size)).mapPartitions { it =>
        val proj = UnsafeProjection.create(types)
        val row = new GenericInternalRow(types.length)
        val (xb, xo) = if (xStr) xDict.value else (null, null)
        val (yb, yo) = if (yStr) yDict.value else (null, null)
        def cols(v: Long, dict: Boolean, b: Array[Byte], o: Array[Long], t: Seq[DataType], nulls: Seq[Boolean]): Array[Any] =
          TesseraPairFilter.cols(v, dict, b, o, t, nulls)
        def typed(v: Any, t: DataType): Any = v match {
          case l: Long => if (t == LongType || t == TimestampType || t == TimestampNTZType) l else if (t == ShortType) l.toShort
                          else l.toInt
          case other => other
        }
        def pass(a: Any, b: Any, op: Int): Boolean = TesseraPairFilter.pass(a, b, op)
        def longSum(hi: Array[Long], lo: Array[Long], q: Int): Long = {
          if (rowSums && hi(q) != (lo(q) >> 63))
            throw org.apache.spark.sql.tessera.TesseraErrors.arithmeticOverflow("long overflow", "try_add")
          lo(q)
        }
        def sum128(hi: Array[Long], lo: Array[Long], q: Int): Decimal = {
          val u = if (hi(q) == (lo(q) >> 63)) java.math.BigInteger.valueOf(lo(q))
                  else java.math.BigInteger.valueOf(hi(q)).shiftLeft(64)
                    .add(new java.math.BigInteger(java.lang.Long.toUnsignedString(lo(q))))
          Decimal(new java.math.BigDecimal(u, s.decScale))
        }
        def decSum(hi: Array[Long], lo: Array[Long], q: Int, ansi: Boolean, suggest: String): Decimal = {
          val d = sum128(hi, lo, q)
          if (d.toJavaBigDecimal.unscaledValue.abs.compareTo(java.math.BigInteger.TEN.pow(math.min(38, s.decArgPrec + 10))) < 0) d
          else if (ansi) throw org.apache.spark.sql.tessera.TesseraErrors.arithmeticOverflow("Overflow in sum of decimals", suggest)
          else null
        }
        def fit(d: Decimal, t: DataType, ansi: Boolean): Decimal = {
          val dt = t.asInstanceOf[DecimalType]
          if (d == null) null else org.apache.spark.sql.tessera.TesseraErrors.toPrecision(d, dt.precision, dt.scale, !ansi)
        }
        it.flatMap { case (a, b, vi, vd, n, cnt, hi) =>
          (0 until n).iterator.flatMap { q =>
            val xc = cols(a(q), xStr, xb, xo, s.xTypes, xNull)
            val yc = cols(b(q), yStr, yb, yo, s.yTypes, if (s.symmetric) xNull else yNull)
            if (!s.filters.forall { case (xi, yi, op) => pass(xc(xi), yc(yi), op) }) Iterator.empty
            else {
              for (col <- types.indices) {
                row.update(col, s.kinds(col) match {
                  case (0, i) => typed(xc(i), s.xTypes(i))
                  case (1, j) => typed(yc(j), s.yTypes(j))
                  case (2, _) if s.two => cnt(q)
                  case (3, _) if s.dec => fit(decSum(hi, vi, q, s.ansi, "try_sum"), types(col), s.ansi)
                  case (3, _) => if (s.sumType == DoubleType) vd(q) else longSum(hi, vi, q)
                  case (4, _) if s.dec =>
                    val sm = decSum(hi, vi, q, s.avgAnsi, "try_avg")
                    if (sm == null) null else fit(sm / Decimal(cnt(q)), types(col), s.avgAnsi)
                  case (4, _) => vd(q) / cnt(q).toDouble
                  case _ if s.dec => fit(decSum(hi, vi, q, s.ansi, "try_sum"), types(col), s.ansi)
                  case _ => if (s.aggType == DoubleType) (if (general) vd(q) else vi(q).toDouble)
                            else (if (general) math.round(vd(q)) else longSum(hi, vi, q))
                })
              }
              Iterator.single(proj(row))
            }
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
    s"TesseraExec(mode=${spec.mode}, agg=${if (spec.vIdx >= 0) "SUM(a.v * b.v)" else if (spec.distinctKeys) "COUNT(DISTINCT k)" else "COUNT(*)"}" +
      (if (spec.minValue > 0) s", prune below ${spec.minValue}" else "") + (if (spec.topk > 0) s", top ${spec.topk}" else "") +
      (if (spec.simMeasure > 0) s", similarity=${spec.simMeasure} >= ${spec.minSim})" else ")")

  override protected def doExecute(): RDD[InternalRow] = {
    val s = spec
    val kStr = s.kType == StringType
    val xStr = s.xType == StringType
    val parts = child.execute().mapPartitions { it =>
      val k = new ArrayBuffer[Long](); val x = new ArrayBuffer[Long](); val v = new ArrayBuffer[Double]()
      val iv = new ArrayBuffer[Long]()
      val kb = new ByteArrayOutputStream(); val xb = new ByteArrayOutputStream()
      val ko = ArrayBuffer[Int](0); val xo = ArrayBuffer[Int](0)
      def int(r: InternalRow, i: Int, t: DataType): Long = t match {
        case LongType | TimestampType | TimestampNTZType => r.getLong(i); case ShortType => r.getShort(i).toLong
        case _ => r.getInt(i).toLong
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
          if (s.vIdx >= 0 && s.vInt) iv += int(r, s.vIdx, s.vType)
          else if (s.vIdx >= 0) v += (s.vType match {
            case DoubleType => r.getDouble(s.vIdx); case FloatType => r.getFloat(s.vIdx).toDouble
            case t => int(r, s.vIdx, t).toDouble
          })
          n += 1
        }
      }
      Iterator(TesseraPart(n, if (kStr) null else k.toArray, if (kStr) kb.toByteArray else null,
        if (kStr) ko.toArray else null, if (xStr) null else x.toArray, if (xStr) xb.toByteArray else null,
        if (xStr) xo.toArray else null, if (s.vIdx >= 0 && !s.vInt) v.toArray else null,
        if (s.vIdx >= 0 && s.vInt) iv.toArray else null))
    }.collect()
    val c = SQLConf.get
    val threads = TesseraConf.threads(c)
    val alive = org.apache.spark.sql.tessera.TesseraStatement.alive(sparkContext)
    val verbose = c.getConfString("spark.tessera.verbose", "false").toBoolean
    val h = TesseraRun.handle(TesseraNative.create(s.vIdx >= 0), "query handle")
    var dk, dx = 0L
    try {
      if (kStr) dk = TesseraRun.handle(TesseraNative.dictNew(), "dictionary")
      if (xStr) dx = TesseraRun.handle(TesseraNative.dictNew(), "dictionary")
      def codes(d: Long, b: Array[Byte], o: Array[Int], n: Int): Array[Long] = {
        val out = new Array[Long](n)
        if (TesseraNative.dictCode(d, b, o, n, out) != 0) throw new IllegalStateException("tessera: out of memory (dictionary)")
        out
      }
      for (p <- parts if p.n > 0) {
        val k = if (kStr) codes(dk, p.kBytes, p.kOffs, p.n) else p.k
        val x = if (xStr) codes(dx, p.xBytes, p.xOffs, p.n) else p.x
        if ((if (s.vInt) TesseraNative.addI64(h, k, x, p.iv, p.n) else TesseraNative.add(h, k, x, p.v, p.n)) != 0)
          throw new IllegalStateException("tessera: out of memory (rows)")
      }
      if (xStr) TesseraNative.setXDict(h, dx)
      if (s.simMeasure > 0) TesseraNative.setSimilarity(h, s.simMeasure, s.minSim)
      if (s.distinctKeys) TesseraNative.setDistinctKeys(h, true)
      TesseraRun.run(h, alive, TesseraNative.run(h, s.mode, s.col0Smaller, s.minValue, s.topk, threads, -1L, verbose))
      if (s.vInt && s.ansi) TesseraEvalModes.check(h, s.vType)
      val general = TesseraNative.general(h)
      val xDict = if (xStr) sparkContext.broadcast((TesseraNative.xStringBytes(h), TesseraNative.xStringOffsets(h)))
                  else null
      val chunks = ArrayBuffer[(Array[Long], Array[Long], Array[Long], Array[Double], Int)]()
      var done = false
      while (!done) {
        TesseraRun.check(alive)
        val a = new Array[Long](1 << 20); val b = new Array[Long](1 << 20)
        val vi = if (general) null else new Array[Long](1 << 20)
        val vd = if (general) new Array[Double](1 << 20) else null
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
          else if (s.xType == LongType || s.xType == TimestampType || s.xType == TimestampNTZType) x
          else if (s.xType == ShortType) x.toShort else x.toInt
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
