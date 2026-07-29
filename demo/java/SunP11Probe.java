// SunPKCS11 兼容性探针 —— 用来**如实回答**"JCA 高层这条路走不走得通"
//
// 结论（JDK 26 实测）：
//   · SunPKCS11 能成功加载本模块，provider 建得起来；
//   · 但它只暴露 1 个服务（KeyStore/PKCS11）——
//     SunPKCS11 的「PKCS#11 机制码 → JCA 算法名」映射表还没有加入 v3.2 的
//     CKM_ML_DSA(0x1d) / CKM_ML_KEM(0x17)；
//   · 于是 KeyPairGenerator.getInstance("ML-DSA", p11Provider) 抛 NoSuchAlgorithmException。
//
// 注意区分两件事：JDK 26 的 JCA **本身**是认识 ML-DSA/ML-KEM 的
// （KeyPairGenerator.getInstance("ML-DSA") 用软件实现可用），
// 缺的是"经由 PKCS#11 provider 去用"这条路。
//
// 运行：
//   java demo/java/SunP11Probe.java <p11.cfg>
import java.security.*;
import java.util.*;
public class SunP11Probe {
  public static void main(String[] a) throws Exception {
    Provider p = Security.getProvider("SunPKCS11");
    if (p == null) { System.out.println("没有 SunPKCS11"); return; }
    p = p.configure(a[0]);
    Security.addProvider(p);
    System.out.println("provider = " + p.getName() + "  版本 " + p.getVersionStr());
    Set<String> types = new TreeSet<>();
    for (Provider.Service s : p.getServices()) types.add(s.getType() + " / " + s.getAlgorithm());
    System.out.println("暴露的服务数 = " + p.getServices().size());
    for (String t : types) System.out.println("   " + t);
    for (String alg : new String[]{"ML-DSA","ML-DSA-65","ML-KEM","ML-KEM-768"}) {
      try { KeyPairGenerator.getInstance(alg, p); System.out.println("  经 P11 的 " + alg + " : 可用"); }
      catch (Exception e) { System.out.println("  经 P11 的 " + alg + " : 不可用 -> " + e.getClass().getSimpleName()); }
    }
  }
}
