package tessera.spark

import java.io.{File, PrintWriter}

import org.apache.spark.sql.SparkSession
import org.apache.spark.sql.types._

object TesseraCheck {
  def main(args: Array[String]): Unit = {
    val Array(bigCsv, out) = args.take(2)
    val spark = SparkSession.builder().appName("tessera-check").getOrCreate()
    val log = new PrintWriter(new File(out))
    def say(s: String): Unit = { println(s); log.println(s); log.flush() }
    say(s"tessera ${TesseraNative.version()} api ${TesseraNative.apiVersion()}")
    val rnd = new scala.util.Random(42)
    val rows = (0 until 60000).map { _ =>
      val k = rnd.nextInt(2000); val x = (math.pow(rnd.nextDouble(), 3) * 500).toInt; val v = 1 + rnd.nextInt(5)
      org.apache.spark.sql.Row(k, x, v.toDouble, k * 1000000007L - 4000000000000000000L,
        x * 999999937L + 3000000000000000000L, s"k$k", s"item-$x")
    }
    val schema = StructType(Seq(StructField("k", IntegerType, false), StructField("x", IntegerType, false),
      StructField("v", DoubleType, false), StructField("kw", LongType, false), StructField("xw", LongType, false),
      StructField("ks", StringType, false), StructField("xs", StringType, false)))
    spark.createDataFrame(spark.sparkContext.parallelize(rows, 8), schema).cache().createOrReplaceTempView("sm")
    val cases = Seq(
      ("int_count", "k", "x", "COUNT(*)", ""), ("int_lt", "k", "x", "COUNT(*)", " AND a.x < b.x"),
      ("int_sum", "k", "x", "SUM(a.v * b.v)", ""), ("wide_count", "kw", "xw", "COUNT(*)", ""),
      ("wide_lt", "kw", "xw", "COUNT(*)", " AND a.xw < b.xw"), ("str_count", "ks", "xs", "COUNT(*)", ""),
      ("str_lt", "ks", "xs", "COUNT(*)", " AND a.xs < b.xs"), ("str_gt", "ks", "xs", "COUNT(*)", " AND a.xs > b.xs"),
      ("strk_int_x", "ks", "x", "COUNT(*)", " AND a.x < b.x"))
    var fails = 0
    for ((name, k, x, agg, cmp) <- cases) {
      val inner = s"SELECT a.$x AS i, b.$x AS j, $agg AS s FROM sm a JOIN sm b ON a.$k = b.$k$cmp GROUP BY a.$x, b.$x"
      val q = s"SELECT count(*), sum(s), sum(CAST(s AS DECIMAL(38, 0)) * hash(i, j)) FROM ($inner) q"
      val res = Seq("true", "false").map { on =>
        spark.conf.set("spark.tessera.enabled", on)
        val df = spark.sql(q)
        val r = df.collect()(0).toSeq.mkString(",")
        val plan = df.queryExecution.executedPlan.toString
        (plan.contains("TesseraExec(mode="), r, plan.contains("TesseraExec"))
      }
      val ok = res(0)._1 && !res(1)._3 && res(0)._2 == res(1)._2
      if (!ok) fails += 1
      say(s"$name tessera_in_plan=${res(0)._1} tessera=(${res(0)._2}) vanilla=(${res(1)._2}) ${if (ok) "same" else "DIFF"}")
    }
    spark.conf.set("spark.tessera.enabled", "true")
    val bigParts = {
      val raw0 = spark.read.schema("k INT, x INT").csv(bigCsv).where("x IS NOT NULL AND k IS NOT NULL")
      raw0.rdd.mapPartitions { it =>
        val k = new scala.collection.mutable.ArrayBuffer[Long](); val x = new scala.collection.mutable.ArrayBuffer[Long]()
        it.foreach { r => k += r.getInt(0).toLong; x += r.getInt(1).toLong }
        Iterator((k.toArray, x.toArray))
      }.collect()
    }
    def nativeRun(cancelAfter: Double): (Double, Double, String) = {
      val h = TesseraNative.create(false)
      for ((k, x) <- bigParts if k.length > 0) TesseraNative.add(h, k, x, null, k.length)
      @volatile var err: String = null
      @volatile var tEnd = 0L
      val th = new Thread(() => { err = TesseraNative.run(h, 0, true, 0L, -1L, 16, -1L, false); tEnd = System.nanoTime() })
      val t0 = System.nanoTime()
      th.start()
      var tCancel = 0L
      if (cancelAfter > 0) { Thread.sleep((cancelAfter * 1000).toLong); tCancel = System.nanoTime(); TesseraNative.cancel(h) }
      th.join()
      TesseraNative.free(h)
      ((tCancel - t0) / 1e9, (tEnd - t0) / 1e9, err)
    }
    val nat = (0 until 3).map(_ => nativeRun(0)._2).sorted
    say(f"native uncancelled ${nat.mkString(" ")} s (median ${nat(1)}%.3f)")
    for (f <- Seq(0.1, 0.4, 0.7)) {
      val (tc, te, err) = nativeRun(nat(1) * f)
      say(f"native cancel at $f%.1f x median: cancel at $tc%.3f s, returned ${te - tc}%.3f s later: ${Option(err).getOrElse("finished")}")
    }
    spark.conf.set("spark.tessera.enabled", "true")
    val raw = spark.read.schema("k INT, x INT").csv(bigCsv).where("x IS NOT NULL AND k IS NOT NULL")
    val nn = StructType(Seq(StructField("k", IntegerType, false), StructField("x", IntegerType, false)))
    spark.createDataFrame(raw.rdd, nn).cache().createOrReplaceTempView("big")
    say(s"big rows ${spark.sql("SELECT count(*) FROM big").collect()(0).getLong(0)}")
    val bq = "SELECT count(*), sum(s) FROM (SELECT a.x AS i, b.x AS j, COUNT(*) AS s FROM big a JOIN big b ON a.k = b.k " +
      "GROUP BY a.x, b.x) q"
    val times = (0 until 3).map { _ =>
      val t0 = System.nanoTime(); spark.sql(bq).collect(); (System.nanoTime() - t0) / 1e9
    }.sorted
    val med = times(1)
    say(f"big uncancelled ${times.mkString(" ")} s (median $med%.3f)")
    for (f <- Seq(0.5, 0.7, 0.9)) {
      @volatile var outcome = "finished"
      @volatile var tEnd = 0L
      val th = new Thread(() => {
        try spark.sql(bq).collect()
        catch { case e: Throwable => outcome = s"${e.getClass.getSimpleName}: ${Option(e.getMessage).getOrElse("").take(80)}" }
        tEnd = System.nanoTime()
      })
      val t0 = System.nanoTime()
      th.start()
      Thread.sleep((med * f * 1000).toLong)
      val tInt = System.nanoTime()
      th.interrupt()
      th.join()
      say(f"cancel at $f%.1f x median: interrupt at ${(tInt - t0) / 1e9}%.3f s, returned ${(tEnd - tInt) / 1e9}%.3f s later: $outcome")
      spark.sparkContext.cancelAllJobs()
    }
    val drain = System.nanoTime() + 120000000000L
    while (spark.sparkContext.statusTracker.getActiveJobIds().nonEmpty && System.nanoTime() < drain) Thread.sleep(100)
    val after = spark.sql(bq).collect()(0).toSeq.mkString(",")
    say(s"after cancellations: $after")
    say(if (fails == 0) "all same" else s"$fails DIFF")
    log.close()
    spark.stop()
    if (fails != 0) sys.exit(1)
  }
}
