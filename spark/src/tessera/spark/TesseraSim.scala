package tessera.spark

import org.apache.spark.sql.DataFrame
import org.apache.spark.sql.catalyst.expressions.{Attribute, AttributeReference, AttributeSet}
import org.apache.spark.sql.catalyst.plans.logical.{LogicalPlan, UnaryNode}
import org.apache.spark.sql.tessera.TesseraRows
import org.apache.spark.sql.types._

case class TesseraSimNode(input: Seq[Attribute], output: Seq[Attribute], measure: Int, minSim: Double, mode: Int,
                          child: LogicalPlan) extends UnaryNode {
  override protected def withNewChildInternal(newChild: LogicalPlan): LogicalPlan = copy(child = newChild)
  override def producedAttributes: AttributeSet = AttributeSet(output)
  def spec: TesseraSpec = {
    def idx(a: Attribute): Int = {
      val i = child.output.indexWhere(_.exprId == a.exprId)
      if (i < 0) throw new IllegalStateException(s"tessera similarity: input column ${a.name} not in the plan")
      i
    }
    TesseraSpec(Seq(0, 1, 2), idx(input(0)), idx(input(1)), if (input.size > 2) idx(input(2)) else -1, input(0).dataType,
      input(1).dataType, if (input.size > 2) input(2).dataType else null, DoubleType, mode, true, measure, minSim)
  }
}

object TesseraSim {
  private def isInt(t: DataType): Boolean = t == IntegerType || t == LongType || t == ShortType

  def similarity(df: DataFrame, k: String, x: String, measure: String, threshold: Double, v: String = null,
                 mode: String = "lt"): DataFrame = {
    val m = measure match {
      case "cosine" => 1; case "jaccard" => 2; case "dice" => 3; case "overlap" => 4
      case _ => throw new IllegalArgumentException("tessera similarity: measure is 'cosine', 'jaccard', 'dice' or 'overlap'")
    }
    val md = mode match {
      case "full" => 0; case "lt" => 1; case "le" => 2
      case _ => throw new IllegalArgumentException("tessera similarity: mode is 'lt', 'le' or 'full'")
    }
    val in = df.select((Seq(k, x) ++ Option(v)).map(df.col): _*)
    val child = in.queryExecution.analyzed
    val attrs = child.output
    if (!isInt(attrs(0).dataType) && attrs(0).dataType != StringType)
      throw new IllegalArgumentException(s"tessera similarity: $k must be short, int, long or string")
    if (!isInt(attrs(1).dataType) && attrs(1).dataType != StringType)
      throw new IllegalArgumentException(s"tessera similarity: $x must be short, int, long or string")
    if (v != null && !isInt(attrs(2).dataType) && attrs(2).dataType != DoubleType && attrs(2).dataType != FloatType)
      throw new IllegalArgumentException(s"tessera similarity: $v must be numeric (short, int, long, float, double)")
    val out = Seq(AttributeReference("x0", attrs(1).dataType, nullable = false)(),
      AttributeReference("x1", attrs(1).dataType, nullable = false)(), AttributeReference("s", DoubleType, nullable = false)())
    TesseraRows.ofRows(df, TesseraSimNode(attrs, out, m, threshold, md, child))
  }
}
