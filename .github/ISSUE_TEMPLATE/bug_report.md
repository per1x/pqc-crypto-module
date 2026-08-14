---
name: Bug report
about: Something does not behave as documented
labels: bug
---

**Where**

- [ ] RTL / simulation
- [ ] Host software (`src/`, `cli/`, PKCS#11)
- [ ] Service layer (`service/`)
- [ ] Board (`board/`) — note that there is one board and it is shared
- [ ] Documentation

**Commit** <!-- git rev-parse --short HEAD -->

**What happened, and what the documentation says should happen**

**Reproduction**

<!-- Smallest thing that shows it: a cocotb test, a ctest target, a payload
     script under board/scripts/, or the exact command line. -->

**Output**

```
```

**Environment** <!-- OS, Verilator / Icarus / Yosys versions, or board image -->

---

If this is a security issue, do not open an issue — see
[SECURITY.md](../../SECURITY.md). Note that the limitations listed in
[docs/SECURITY.md](../../docs/SECURITY.md) are known and documented.
