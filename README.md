# PoZS: An Intelligent Resource Management and Data Placement Scheme for Next-generation Mobile NAND Solution

PoZS is a performance-oriented zone scheduling framework for Zoned Namespace (ZNS) SSDs.
It is implemented on top of [NVMeVirt](https://github.com/snu-csl/nvmevirt), a software-defined virtual NVMe device implemented as a Linux kernel module.

PoZS exploits the asymmetric characteristics of TLC NAND page types and manages zone allocation according to page-type-aware scheduling policies.
The current implementation is configured to run in ZNS mode by default and is designed for evaluation with the F2FS file system.

