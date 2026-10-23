package tessera.spark

import java.io.{File, PrintWriter}

import org.apache.spark.sql.SparkSession
import org.apache.spark.sql.functions._

object TesseraSimCheck {
  def main(args: Array[String]): Unit = {
    val spark = SparkSession.builder().appName("tessera-sim-check").getOrCreate()
    val log = new PrintWriter(new File(args(0)))
    def say(s: String): Unit = { println(s); log.println(s); log.flush() }
    say(s"tessera ${TesseraNative.version()} api ${TesseraNative.apiVersion()}")
    import spark.implicits._
    val rnd = new scala.util.Random(17)
    val rows = (0 until 40000).map { _ =>
      val k = rnd.nextInt(1500); val x = (rnd.nextInt(600) * rnd.nextInt(600)) / 900
      (k, x, (1 + rnd.nextInt(3)).toDouble, s"i-$x")
    }
    val st = rows.toDF("k", "x", "v", "xs").cache()
    val su = st.select("k", "x", "xs").distinct().cache()
    st.createOrReplaceTempView("st"); su.createOrReplaceTempView("su")
    spark.conf.set("spark.tessera.enabled", "false")
    def sim(m: String): String = m match {
      case "cosine" => "c.n / sqrt(di.n * dj.n)"; case "jaccard" => "c.n / (di.n + dj.n - c.n)"
      case "dice" => "2.0 * c.n / (di.n + dj.n)"; case _ => "c.n / least(di.n, dj.n)"
    }
    var fails = 0
    for ((tbl, src, agg, v, col) <- Seq(("su", "su", "COUNT(*)", null, "x"), ("st", "st", "COUNT(*)", null, "x"),
                                        ("sv", "st", "SUM(a.v * b.v)", "v", "x"), ("sx", "su", "COUNT(*)", null, "xs"));
         m <- Seq("cosine", "jaccard", "dice", "overlap"); th <- Seq(0.05, if (m == "jaccard") 0.1 else 0.2);
         mode <- Seq("lt", "le")) {
      val cmp = if (mode == "lt") "<" else "<="
      val ref = spark.sql(s"WITH c AS (SELECT a.$col i, b.$col j, CAST($agg AS DOUBLE) n FROM $src a JOIN $src b ON a.k = b.k AND a.$col $cmp b.$col GROUP BY a.$col, b.$col), " +
        s"d AS (SELECT a.$col x, CAST($agg AS DOUBLE) n FROM $src a JOIN $src b ON a.k = b.k AND a.$col = b.$col GROUP BY a.$col) " +
        s"SELECT c.i, c.j, ${sim(m)} s FROM c JOIN d di ON di.x = c.i JOIN d dj ON dj.x = c.j").where($"s" >= th).cache()
      val t = TesseraSim.similarity(spark.table(src), "k", col, m, th, v, mode).cache()
      val nt = t.count(); val nr = ref.count()
      val d = t.select("x0", "x1").exceptAll(ref.select("i", "j")).count() + ref.select("i", "j").exceptAll(t.select("x0", "x1")).count()
      val err = t.join(ref, t("x0") === ref("i") && t("x1") === ref("j")).select(max(abs(t("s") - ref("s")) / t("s"))).first()
      val ok = nt == nr && d == 0 && nt > 0 && (err.isNullAt(0) || err.getDouble(0) < 1e-12)
      if (!ok) fails += 1
      say(s"${tbl}_${m}_${th}_$mode rows $nt $nr pairs_differ $d max_rel_err ${err.get(0)} ${if (ok) "same" else "DIFF"}")
      t.unpersist(); ref.unpersist()
    }
    val q = TesseraSim.similarity(spark.table("su"), "k", "x", "cosine", 0.05).where($"s" > 0.1 && $"x0" < 50)
    say(s"plan has TesseraExec: ${q.queryExecution.executedPlan.toString.contains("TesseraExec")}")
    val composed = q.count()
    val direct = TesseraSim.similarity(spark.table("su"), "k", "x", "cosine", 0.05).collect().count(r => r.getDouble(2) > 0.1 && r.getInt(0) < 50)
    say(s"composed filter $composed $direct ${if (composed == direct) "same" else "DIFF"}")
    if (composed != direct) fails += 1
    say(if (fails == 0) "all same" else s"$fails DIFF")
    log.close()
    spark.stop()
    if (fails != 0) sys.exit(1)
  }
}
