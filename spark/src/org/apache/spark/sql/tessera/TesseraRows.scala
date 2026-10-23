package org.apache.spark.sql.tessera

import org.apache.spark.sql.DataFrame
import org.apache.spark.sql.catalyst.plans.logical.LogicalPlan
import org.apache.spark.sql.classic

object TesseraRows {
  def ofRows(like: DataFrame, plan: LogicalPlan): DataFrame =
    classic.Dataset.ofRows(like.sparkSession.asInstanceOf[classic.SparkSession], plan)
}
