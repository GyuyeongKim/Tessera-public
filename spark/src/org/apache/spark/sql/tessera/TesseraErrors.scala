package org.apache.spark.sql.tessera

import org.apache.spark.sql.errors.QueryExecutionErrors

object TesseraErrors {
  def arithmeticOverflow(message: String, suggestedFunc: String): ArithmeticException =
    QueryExecutionErrors.arithmeticOverflowError(message, suggestedFunc, null)
}
