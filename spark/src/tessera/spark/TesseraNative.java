package tessera.spark;

public final class TesseraNative {
  static {
    System.load(System.getProperty("tessera.jni"));
  }

  private TesseraNative() {}

  public static native int apiVersion();

  public static native String version();

  public static native long create(boolean hasValue);

  public static native long createCross(boolean hasValue);

  public static native int addY(long h, long[] k, long[] y, double[] w, int n);

  public static native int add(long h, long[] k, long[] x, double[] v, int n);

  public static native long dictNew();

  public static native int dictCode(long d, byte[] bytes, int[] offs, int n, long[] out);

  public static native void dictFree(long d);

  public static native void setXDict(long h, long d);

  public static native byte[] xStringBytes(long h);

  public static native long[] xStringOffsets(long h);

  public static native void setYDict(long h, long d);

  public static native byte[] yStringBytes(long h);

  public static native long[] yStringOffsets(long h);

  public static native String run(long h, int mode, boolean col0Smaller, long minValue, long topk, int threads,
                                  long frontier, boolean verbose);

  public static native void cancel(long h);
  public static native void setSimilarity(long h, int measure, double minSim);

  public static native int next(long h, long[] x0, long[] x1, long[] vi, double[] vd);

  public static native boolean general(long h);

  public static native void free(long h);
}
