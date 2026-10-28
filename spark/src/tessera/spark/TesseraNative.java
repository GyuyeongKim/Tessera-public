package tessera.spark;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;

public final class TesseraNative {
  static {
    String path = System.getProperty("tessera.jni");
    System.load(path != null ? path : bundled());
  }

  private static String bundled() {
    String os = System.getProperty("os.name").toLowerCase().startsWith("mac") ? "darwin" : "linux";
    String arch = System.getProperty("os.arch");
    String lib = os.equals("darwin") ? "libtessera_jni.dylib" : "libtessera_jni.so";
    String res = "/tessera/native/" + os + "-" + (arch.equals("amd64") ? "x86_64" : arch) + "/" + lib;
    try (InputStream in = TesseraNative.class.getResourceAsStream(res)) {
      if (in == null) {
        throw new UnsatisfiedLinkError("tessera: the jar has no " + res + " for this platform; set -Dtessera.jni");
      }
      Path tmp = Files.createTempFile("tessera_jni", lib.substring(lib.lastIndexOf('.')));
      tmp.toFile().deleteOnExit();
      Files.copy(in, tmp, StandardCopyOption.REPLACE_EXISTING);
      return tmp.toString();
    } catch (IOException e) {
      throw new UnsatisfiedLinkError("tessera: cannot extract " + res + ": " + e.getMessage());
    }
  }

  private TesseraNative() {}

  public static native int apiVersion();

  public static native boolean supported();

  public static native String version();

  public static native long create(boolean hasValue);

  public static native long createCross(boolean hasValue);

  public static native int addY(long h, long[] k, long[] y, double[] w, int n);

  public static native int add(long h, long[] k, long[] x, double[] v, int n);

  public static native int addI64(long h, long[] k, long[] x, long[] v, int n);

  public static native int addYI64(long h, long[] k, long[] y, long[] w, int n);

  public static native int overflow(long h, int productBits, int sumBits);

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

  public static native int lastStatus();

  public static native void cancel(long h);
  public static native void setSimilarity(long h, int measure, double minSim);

  public static native int next(long h, long[] x0, long[] x1, long[] vi, double[] vd);

  public static native boolean general(long h);

  public static native void free(long h);
}
