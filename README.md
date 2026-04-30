# PoZS: An Intelligent Resource Management and Data Placement Scheme for Next-generation Mobile NAND Solution

PoZS is a performance-oriented zone scheduling framework for Zoned Namespace (ZNS) SSDs.
It is implemented on top of [NVMeVirt](https://github.com/snu-csl/nvmevirt), a software-defined virtual NVMe device implemented as a Linux kernel module.

PoZS exploits the asymmetric characteristics of TLC NAND page types and manages zone allocation according to page-type-aware scheduling policies.
The current implementation is configured to run in ZNS mode by default and is designed for evaluation with the F2FS file system.

## Installation

### Prerequisites

PoZS was implemented and tested on Linux kernel 5.16.0.

Recommended environment:

| Item | Description |
|:---|:---|
| OS | Ubuntu 20.04.6 LTS or compatible Linux distribution |
| Kernel | Linux kernel 5.16.0 |
| Emulator | NVMeVirt-based virtual NVMe device |
| Device mode | ZNS |
| File system | F2FS |

### Build PoZS

1. Clone the repository:

```bash
git clone https://github.com/noslab/PoZS.git
cd PoZS
```

2. Build and insert the PoZS module:

```bash
sudo ./module.sh
```

The `module.sh` script builds and inserts the NVMeVirt-based PoZS kernel module.  
The module is automatically configured in ZNS mode.

By default, the module is configured as follows:

| Parameter | Value |
|:---|:---|
| Device mode | ZNS |
| Capacity | 40 GB |
| Number of cores | 2 |
| File system | F2FS |

## Configuration

The detailed SSD configuration, including flash geometry and latency parameters, can be found in `ssd_config.h`.

### Default Script Configuration

The default configuration used by `module.sh` is as follows:

| Parameter | Description | Default |
|:---|:---|:---|
| Device mode | NVMeVirt execution mode | ZNS |
| Capacity | Emulated ZNS SSD capacity | 40 GB |
| Cores | Number of cores used by the module | 2 |

### SSD Configuration

| Item | Specification | Unit |
|:---|:---|:---|
| Capacity | 42 GiB | - |
| Flash page size | 32 KiB | - |
| Block size | 32 MiB | - |
| Zone size | 1 GiB | - |
| Number of zones | 42 | - |
| Flash blocks per zone | 32 | - |
| LSB read / write latency | 50 / 500 | us/page |
| CSB read / write latency | 100 / 2,000 | us/page |
| MSB read / write latency | 150 / 5,500 | us/page |
| Erase latency | 95 | ms/zone |

## Running

A typical execution flow is as follows:

```bash
# Clone the repository
git clone https://github.com/noslab/PoZS.git
cd PoZS

# Build and insert the PoZS module in ZNS mode
sudo ./module.sh

# Check the created NVMe device
lsblk

# Modify the device and mount path for your system
vi f2fs.sh

# Create namespace, format with F2FS, and mount
sudo ./f2fs.sh
```
