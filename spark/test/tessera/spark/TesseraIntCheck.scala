package tessera.spark

import java.io.{File, PrintWriter}

import org.apache.spark.sql.{Row, SparkSession}
import org.apache.spark.sql.types._

object TesseraIntCheck {
  def main(args: Array[String]): Unit = {
    val spark = SparkSession.builder().appName("tessera-int-check").getOrCreate()
    val log = new PrintWriter(new File(args(0)))
    def say(s: String): Unit = { println(s); log.println(s); log.flush() }
    say(s"tessera ${TesseraNative.version()} api ${TesseraNative.apiVersion()}")
    def table(name: String, t: DataType, rows: Seq[(Long, Long, Long)]): Unit = {
      val conv: Long => Any = t match {
        case LongType => v => v; case IntegerType => v => v.toInt; case ShortType => v => v.toShort
        case FloatType => v => v.toFloat; case _ => v => v.toDouble
      }
      val schema = StructType(Seq(StructField("k", LongType, false), StructField("x", LongType, false),
        StructField("v", t, false)))
      spark.createDataFrame(spark.sparkContext.parallelize(rows.map { case (k, x, v) => Row(k, x, conv(v)) }, 4), schema)
        .createOrReplaceTempView(name)
    }
    val rnd = new scala.util.Random(7)
    table("big", LongType, Seq((1L, 1L, 134217729L), (1L, 2L, 3L), (2L, 1L, 1L), (2L, 2L, 1L)) ++
      (0 until 40).map(i => (3L + i, 3L, 1L)))
    table("pair", LongType, Seq((1L, 1L, 100000001L), (1L, 2L, 100000001L)))
    table("ov", IntegerType, (0 until 1000).map(i => ((i % 10).toLong, (i % 7).toLong, 50000L)))
    table("sg", IntegerType, (0 until 5000).map(_ => (rnd.nextInt(300).toLong, rnd.nextInt(40).toLong,
      (rnd.nextInt(2001) - 1000).toLong)))
    table("sh", ShortType, (0 until 300).map(i => ((i % 5).toLong, (i % 3).toLong, 200L)))
    table("hs", LongType, (0 until 64).flatMap(k => Seq((k.toLong, 1L, 3000000000L), (k.toLong, 2L, 3000000000L))))
    table("fl", FloatType, (0 until 100).map(i => ((i % 10).toLong, (i % 3).toLong, (i % 3 + 1).toLong)))
    val cases = Seq(
      ("long_exact", "SELECT a.x i, b.x j, SUM(a.v * b.v) s FROM big a JOIN big b ON a.k = b.k GROUP BY a.x, b.x", true, true),
      ("long_pair_lt", "SELECT a.x i, b.x j, SUM(a.v * b.v) s FROM pair a JOIN pair b ON a.k = b.k AND a.x < b.x GROUP BY a.x, b.x", true, true),
      ("int_overflow", "SELECT a.x i, b.x j, SUM(a.v * b.v) s FROM ov a JOIN ov b ON a.k = b.k GROUP BY a.x, b.x", true, true),
      ("int_signed", "SELECT a.x i, b.x j, SUM(a.v * b.v) s FROM sg a JOIN sg b ON a.k = b.k GROUP BY a.x, b.x", true, true),
      ("short_declined", "SELECT a.x i, b.x j, SUM(a.v * b.v) s FROM sh a JOIN sh b ON a.k = b.k GROUP BY a.x, b.x", true, false),
      ("long_sum_overflow", "SELECT a.x i, b.x j, SUM(a.v * b.v) s FROM hs a JOIN hs b ON a.k = b.k AND a.x < b.x GROUP BY a.x, b.x", true, true),
      ("long_wrap_legacy", "SELECT a.x i, b.x j, SUM(a.v * b.v) s FROM hs a JOIN hs b ON a.k = b.k AND a.x < b.x GROUP BY a.x, b.x", false, true),
      ("int_legacy_declined", "SELECT a.x i, b.x j, SUM(a.v * b.v) s FROM ov a JOIN ov b ON a.k = b.k GROUP BY a.x, b.x", false, false),
      ("float_declined", "SELECT a.x i, b.x j, SUM(a.v * b.v) s FROM fl a JOIN fl b ON a.k = b.k GROUP BY a.x, b.x", true, false),
      ("cross_long", "SELECT a.x i, b.x j, SUM(a.v * b.v) s FROM big a JOIN pair b ON a.k = b.k GROUP BY a.x, b.x", true, true))
    var fails = 0
    for ((name, q, ansi, take) <- cases) {
      spark.conf.set("spark.sql.ansi.enabled", ansi.toString)
      val res = Seq("true", "false").map { on =>
        spark.conf.set("spark.tessera.enabled", on)
        val df = spark.sql(s"$q ORDER BY i, j")
        try {
          val r = df.collect().map(_.toSeq.mkString(":")).mkString(",")
          (df.queryExecution.executedPlan.toString.contains("TesseraExec"), r)
        } catch {
          case e: Throwable =>
            var c: Throwable = e
            while (c != null && !c.isInstanceOf[ArithmeticException]) c = c.getCause
            (df.queryExecution.executedPlan.toString.contains("TesseraExec"),
              if (c != null) s"${c.getClass.getSimpleName}: ${String.valueOf(c.getMessage).linesIterator.toSeq.headOption.getOrElse("")}"
              else s"other error ${e.getClass.getSimpleName}: ${e.getMessage.take(120)}")
        }
      }
      val ok = res(0)._1 == take && !res(1)._1 && res(0)._2 == res(1)._2
      if (!ok) fails += 1
      say(s"$name ansi=$ansi tessera_in_plan=${res(0)._1} (want $take) tessera=(${res(0)._2.take(200)}) " +
        s"vanilla=(${res(1)._2.take(200)}) ${if (ok) "same" else "DIFF"}")
    }
    say(s"cases ${cases.size} failures $fails")
    log.close()
    spark.stop()
    if (fails > 0) System.exit(1)
  }
}
