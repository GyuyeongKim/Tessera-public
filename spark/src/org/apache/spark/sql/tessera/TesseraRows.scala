package org.apache.spark.sql.tessera

import org.apache.spark.SparkContext
import org.apache.spark.sql.DataFrame
import org.apache.spark.sql.catalyst.plans.logical.LogicalPlan
import org.apache.spark.sql.classic
import org.apache.spark.sql.execution.SQLExecution

object TesseraRows {
  def ofRows(like: DataFrame, plan: LogicalPlan): DataFrame =
    classic.Dataset.ofRows(like.sparkSession.asInstanceOf[classic.SparkSession], plan)
}

object TesseraStatement {
  def alive(sc: SparkContext): () => Boolean = {
    val id = sc.getLocalProperty(SQLExecution.EXECUTION_ID_KEY)
    if (id == null) () => true
    else {
      val e = id.toLong
      () => SQLExecution.getQueryExecution(e) != null
    }
  }
}
