package tessera.spark

import java.io.{File, PrintWriter}

import org.apache.spark.sql.{Row, SparkSession}
import org.apache.spark.sql.types._

object TesseraCrossCheck {
  def main(args: Array[String]): Unit = {
    val spark = SparkSession.builder().appName("tessera-cross-check").getOrCreate()
    val log = new PrintWriter(new File(args(0)))
    def say(s: String): Unit = { println(s); log.println(s); log.flush() }
    say(s"tessera ${TesseraNative.version()} api ${TesseraNative.apiVersion()}")
    val rnd = new scala.util.Random(7)
    def table(n: Int, keys: Int, items: Int, pw: Double, name: String, vals: Int): Unit = {
      val rows = (0 until n).map { _ =>
        val k = rnd.nextInt(keys); val x = (math.pow(rnd.nextDouble(), pw) * items).toInt
        Row(k, x, (1 + rnd.nextInt(vals)).toDouble, k * 1000000007L - 4000000000000000000L, x * 999999937L + 7L,
          s"k$k", s"$name-$x", (rnd.nextInt(5) * 0.5 - 0.5))
      }
      val schema = StructType(Seq(StructField("k", IntegerType, false), StructField("x", IntegerType, false),
        StructField("v", DoubleType, false), StructField("kw", LongType, false), StructField("xw", LongType, false),
        StructField("ks", StringType, false), StructField("xs", StringType, false), StructField("f", DoubleType, false)))
      spark.createDataFrame(spark.sparkContext.parallelize(rows, 8), schema).cache().createOrReplaceTempView(name)
    }
    table(50000, 3000, 1000, 3.0, "r", 4)
    table(40000, 3000, 800, 2.0, "s", 3)
    val cases = Seq(
      ("int_count", "SELECT a.x AS i, b.x AS j, COUNT(*) AS c FROM r a JOIN s b ON a.k = b.k GROUP BY a.x, b.x"),
      ("int_sum", "SELECT a.x AS i, b.x AS j, SUM(a.v * b.v) AS c FROM r a JOIN s b ON a.k = b.k GROUP BY a.x, b.x"),
      ("general_sum", "SELECT a.x AS i, b.x AS j, SUM(a.f * b.f) AS c FROM r a JOIN s b ON a.k = b.k GROUP BY a.x, b.x"),
      ("wide", "SELECT a.xw AS i, b.xw AS j, COUNT(*) AS c FROM r a JOIN s b ON a.kw = b.kw GROUP BY a.xw, b.xw"),
      ("str", "SELECT a.xs AS i, b.xs AS j, COUNT(*) AS c FROM r a JOIN s b ON a.ks = b.ks GROUP BY a.xs, b.xs"),
      ("int_x_str_y", "SELECT a.x AS i, b.xs AS j, SUM(a.v * b.v) AS c FROM r a JOIN s b ON a.ks = b.ks GROUP BY a.x, b.xs"),
      ("reversed", "SELECT b.x AS i, a.x AS j, COUNT(*) AS c FROM r a JOIN s b ON b.k = a.k GROUP BY b.x, a.x"),
      ("same_table", "SELECT a.x AS i, b.xs AS j, COUNT(*) AS c FROM r a JOIN r b ON a.k = b.k GROUP BY a.x, b.xs"))
    var fails = 0
    for ((name, inner) <- cases) {
      val q = s"SELECT count(*), sum(c), sum(CAST(c AS DECIMAL(38, 4)) * (hash(i, j) % 1009)) FROM ($inner) q"
      val res = Seq("true", "false").map { on =>
        spark.conf.set("spark.tessera.enabled", on)
        val df = spark.sql(q)
        val r = df.collect()(0).toSeq.mkString(",")
        val plan = df.queryExecution.executedPlan.toString
        (if (on == "true") plan.contains("TesseraExec(cross") else plan.contains("TesseraExec"), r)
      }
      val ok = res(0)._1 && !res(1)._1 && res(0)._2 == res(1)._2
      if (!ok) fails += 1
      say(s"$name tessera_in_plan=${res(0)._1} tessera=(${res(0)._2}) vanilla=(${res(1)._2}) ${if (ok) "same" else "DIFF"}")
    }
    say(if (fails == 0) "all same" else s"$fails DIFF")
    log.close()
    spark.stop()
    if (fails != 0) sys.exit(1)
  }
}
