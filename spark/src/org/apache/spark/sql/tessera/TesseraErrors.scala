package org.apache.spark.sql.tessera

import org.apache.spark.sql.errors.QueryExecutionErrors
import org.apache.spark.sql.types.Decimal

object TesseraErrors {
  def arithmeticOverflow(message: String, suggestedFunc: String): ArithmeticException =
    QueryExecutionErrors.arithmeticOverflowError(message, suggestedFunc, null)
  def toPrecision(d: Decimal, precision: Int, scale: Int, nullOnOverflow: Boolean): Decimal =
    d.toPrecision(precision, scale, Decimal.ROUND_HALF_UP, nullOnOverflow)
}
