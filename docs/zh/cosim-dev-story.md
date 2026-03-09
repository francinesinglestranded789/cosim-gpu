[English](../en/cosim-dev-story.md)

# 只用两天：我用 Claude 把十万块的 MI300X GPU 搬进了 QEMU

> AMD Instinct MI300X，304 个计算单元，192GB HBM3 显存，单卡零售价超过 10 万人民币。
> 现在，你只需要一台普通的 x86 Linux 机器，就能在 QEMU 上跑起完整的 ROCm/HIP 工作负载。

## 01
缘起：当 gem5 的启动时间比调试还长

我做 GPU 模拟器已经有一段时间了。gem5 有 MI300X 的设备模型，也有全系统仿真的能力，但它的 KVM 快进模式仍然很慢——一次 Linux 启动要等 5 分钟，驱动加载再等 5 分钟，每次调试一个 MMIO 寄存器的问题都意味着 10 分钟的空白等待。

我一直想做一件事：让 QEMU 跑 Linux 和 amdgpu 驱动，gem5 只负责 GPU 计算模型，中间用某种 IPC 桥接起来。这样 QEMU 用 KVM 跑 CPU 部分，速度接近原生；gem5 只处理 GPU 的 MMIO/Doorbell/DMA，可以专注在计算仿真的精度上。

这个想法听起来不复杂，但实际做起来涉及到 QEMU PCIe 设备模型、gem5 SimObject 架构、Linux amdgpu 驱动的初始化流程、GART 地址翻译、共享内存文件偏移量对齐、Unix 域套接字的边沿触发语义——这些东西的交叉点上全是坑。

2026 年 3 月 6 日早上，我打开了 Claude Code，开始了这个项目。到 3 月 8 日凌晨，第一个 HIP 向量加法测试在联合仿真环境下跑出了 `PASSED!`。

这篇文章记录了整个过程中踩过的坑和关键决策。

---

## 02
架构：一句话版本

```
+-----------------------------+       +----------------------------+
|  QEMU  (Q35 + KVM)          |       |  gem5  (Docker)            |
|  +-----------------------+  |       |  +----------------------+  |
|  | Guest Linux           |  |       |  | MI300X GPU Model     |  |
|  | amdgpu driver         |  |       |  |  Shader / CU / SDMA  |  |
|  | ROCm 7.0 / HIP        |  |       |  |  PM4 / Ruby caches   |  |
|  +----------+------------+  |       |  +---------+------------+  |
|             |               |       |            |               |
|  +----------v------------+  |       |  +---------v------------+  |
|  | vfio-user-pci (内置)  |<-------->|  | MI300XVfioUser       |  |
|  +-----------------------+  |vfio-  |  +----------------------+  |
|                             |user   |                            |
+-----------------------------+socket +----------------------------+
        |                                       |
        v                                       v
  /dev/shm/cosim-guest-ram            /dev/shm/mi300x-vram
  (shared guest RAM)                  (shared GPU VRAM)
```

> **后端选择**：默认使用 vfio-user 后端（`MI300XVfioUser`），QEMU 侧使用内置的 `vfio-user-pci` 设备，无需自定义 QEMU 代码。也支持 legacy 后端（`MI300XGem5Cosim` + 自定义 `mi300x-gem5` QEMU 设备），通过 `--cosim-backend=legacy` 切换。

QEMU 这边是一个完整的 Q35 虚拟机，跑 Ubuntu 24.04 + ROCm 7.0 + amdgpu 驱动。vfio-user 后端使用 QEMU 内置的 `vfio-user-pci` 设备，通过标准 vfio-user 协议把所有 MMIO 读写和 Doorbell 写操作转发给 gem5。

gem5 这边跑的是 MI300X 的 GPU 设备模型——Shader、CU 阵列、PM4 命令处理器、SDMA 引擎、Ruby 缓存层次结构——但**没有 Linux 内核**。它用 `StubWorkload` 空壳启动，只等 QEMU 通过 socket 发来 MMIO 请求。

Guest 物理内存和 GPU VRAM 各有一块共享内存文件（`/dev/shm/`），QEMU 和 gem5 都能直接 mmap，实现零拷贝 DMA。

BAR 布局必须严格匹配 amdgpu 驱动的硬编码预期：

| BAR | 内容 | 大小 | 通信方式 |
|-----|------|------|----------|
| BAR0+1 | VRAM | 16 GiB | 共享内存 |
| BAR2+3 | Doorbell | 4 MiB | Socket 转发 |
| BAR4 | MSI-X | 256 vectors | QEMU 本地 |
| BAR5 | MMIO 寄存器 | 512 KiB | Socket 转发 |

---

## 03
起步：从零开始写 PCIe 设备

6 号早上 6 点半，我让 Claude 帮我写了 QEMU 侧的 `mi300x_gem5.c`。这是一个标准的 QEMU PCIe 设备，但有几个特殊的地方：

1. **六个 BAR**，其中三个需要 64 位地址空间（VRAM 16GB 不可能放在 4G 以下）
2. **两条 socket 连接**：一条同步（MMIO 请求/响应），一条异步（中断和 DMA 事件）
3. **MSI-X 支持**：256 个中断向量，gem5 通过 event socket 通知 QEMU 触发 `msix_notify()`

gem5 侧的 `MI300XGem5Cosim` SimObject 稍微复杂一点——它是一个 socket 服务器，监听来自 QEMU 的连接，接收 MMIO 消息后分发给 `AMDGPUDevice` 处理，再把结果发回去。

第一版代码大约 1500 行（QEMU 700 行 + gem5 800 行），结构清晰但全是 bug。

---

## 04
踩坑：从 SIGIO 死锁到 GART 翻译

### Bug #1：SIGIO 边沿触发死锁——最阴险的问题

gem5 的事件系统使用 `FASYNC`/`SIGIO` 来监听 socket 上的数据。这是**边沿触发**的——当 socket 缓冲区从空变非空时，内核发一次 `SIGIO`，仅此一次。

问题出在 amdgpu 驱动的寄存器访问模式上。驱动经常先写 INDEX 寄存器（选择要访问哪个内部寄存器），然后立即读 DATA 寄存器（拿到值）。write 是 fire-and-forget 的，read 是阻塞等待响应的。当这两条消息背靠背到达 gem5 的 socket 缓冲区时，只会触发一次 SIGIO。

我最初的 `handleClientData()` 每次只读一条消息。结果：gem5 读了 write 消息，处理完毕，然后就傻等下一次 SIGIO。但 read 消息已经在缓冲区里了，不会再有新的 SIGIO 来唤醒它。QEMU 那边死等 read 响应。**完美死锁。**

gem5 处理了 15 条消息后就永远挂住了。

修复方法很简单——把单次读取改成排空循环：

```cpp
void MI300XGem5Cosim::handleClientData(int fd) {
    struct pollfd pfd;
    do {
        CosimMsgHeader msg;
        if (!recvAll(fd, &msg, COSIM_MSG_HDR_SIZE)) {
            closeClient(fd); return;
        }
        processMessage(fd, msg);
        pfd = {fd, POLLIN, 0};
    } while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN));
}
```

修完这个之后，MMIO 消息数从 15 跳到了 **35,181**。驱动初始化一路推进到了 PSP 固件加载阶段。

**教训：任何基于 FASYNC 的 I/O handler 都必须排空所有待处理数据。这在 PCIe 间接寄存器访问的场景下是必然的。**

### Bug #2：ip_block_mask——文档骗人

amdgpu 驱动有一个 `ip_block_mask` 参数，用来控制哪些 IP 块需要初始化。cosim 模式下不需要 PSP（安全处理器）和 SMU（电源管理），需要禁用它们。

我最初用的是 `0x6f`，觉得禁用了 PSP（枚举值 4）和保留了其他。结果 PSP 还是被初始化了，加载固件时报 `-EINVAL` 然后整个 GPU init 失败。

花了好一阵子才搞明白：`ip_block_mask` 的位对应的是 **IP discovery 的检测顺序索引**，不是 `amd_ip_block_type` 枚举值。MI300X 的检测顺序是：

```
0: soc15_common   1: gmc_v9_0    2: vega20_ih
3: psp            4: smu         5: gfx_v9_4_3
6: sdma_v4_4_2    7: vcn_v4_0_3  8: jpeg_v4_0_3
```

PSP 在枚举值里是 4，但在检测顺序里是 3。`0x6f` = `0110_1111` 禁用的是索引 4（smu），但索引 3（psp）还是被启用了。正确的值是 `0x67` = `0110_0111`，同时禁用索引 3 和 4。

**教训：amd_shared.h 的枚举值和驱动实际使用的位掩码之间没有对应关系。只有 dmesg 的检测日志才是真相。**

### Bug #3：共享内存偏移量——两个系统的内存观不一致

这个 bug 最诡异。GART 页表项读出来全是零，PM4 命令处理器一直读到 opcode 0x0（NOP），无限循环。

问题出在 QEMU Q35 和 gem5 对内存拆分方式的不同。配置 8GB RAM 时：

- **QEMU Q35** 硬编码 `below_4g = 2 GiB`（当 `ram_size >= 0xB0000000`），上方 6GB 放在文件偏移 2G 处
- **gem5** 默认 `below_4g = 3 GiB`，上方 5GB 放在文件偏移 3G 处

两边 mmap 同一个共享内存文件，但对"第 4G 以上的内存在文件的哪个偏移"意见不一致。gem5 从偏移 3G 处读 GART 页表——那里全是零，因为 QEMU 把数据写在了偏移 2G 处。

修复：在 `mi300_cosim.py` 里完全复制 Q35 的拆分逻辑。

**教训：共享 memory-backend-file 时，双方必须在每个范围的文件偏移量上达成一致，不仅仅是总大小。**

### Bug #4：VRAM 地址被错误地走了 GART 翻译

PM4 的 `RELEASE_MEM` 和 SDMA 的 rptr 回写，目标地址有时候指向 VRAM（地址 < 16GiB）。原来的代码把所有地址都扔进 `getGARTAddr()` 做翻译，但 VRAM 地址在 GART 里没有对应的页表项，翻译失败 861,000 多次，最后内存耗尽段错误。

修复用了三层防护：

1. **PM4 层**：`writeData()` / `releaseMem()` 检查 `isVRAMAddress(addr)`，VRAM 写直接走设备内存
2. **SDMA 层**：rptr 回写对 VRAM 地址跳过 `getGARTAddr()`
3. **GART 兜底**：未映射的 GART 页映射到 `paddr=0`（sink），不产生 fault

---

## 05
验证：HIP 向量加法 PASSED

3 月 8 日凌晨，所有 bug 修完，驱动加载正常，`rocm-smi` 看到了 MI300X (0x74a0)，`rocminfo` 报告 gfx942 架构、320 个 CU。

在 Guest 里写了一个最简单的 HIP 测试——四个元素的向量加法：

```cpp
__global__ void add(int *a, int *b, int *c, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) c[i] = a[i] + b[i];
}
```

编译，运行：

```
Result: 11 22 33 44
PASSED!
```

`{1+10, 2+20, 3+30, 4+40}` = `{11, 22, 33, 44}`。hipMalloc、hipMemcpy（host-to-device / device-to-host）、kernel dispatch、hipDeviceSynchronize 全部正常返回。MSI-X 中断从 gem5 通过 event socket 转发给 QEMU，QEMU 触发 `msix_notify()`，guest 的 IH handler 正确处理——整个中断链路首次端到端跑通。

这是 gem5 作为"远程 GPU"被 QEMU guest 里的真实 amdgpu 驱动驱动起来做计算的最佳实践。

---

## 06
协作：不是代码工具，而是系统搭档

整个开发过程在一个巨型对话里完成，上下文用完了就续上。工作流是这样的：

1. **我提供原始的终端输出**：dmesg 日志、gem5 panic 信息、socket 通信的 hexdump
2. **Claude 分析输出**，搜索 gem5/QEMU/Linux 内核源码定位根因
3. **Claude 提出并实现修复**——直接编辑 gem5 C++ 代码、QEMU C 代码、Python 配置、Shell 脚本
4. **后台构建**：gem5 编译约 30 分钟，QEMU 约 5 分钟，磁盘镜像约 40 分钟——这些都在后台跑
5. **我测试，贴新的输出**，循环继续

Claude 在这个项目里的角色不是"帮我写代码的工具"，而更像一个**对 gem5 和 QEMU 内部机制有深入了解的协作者**。几个典型场景：

- **SIGIO 死锁**：我只贴了"gem5 处理 15 条消息后挂住"，Claude 立刻定位到 FASYNC 的边沿触发语义，给出了排空循环的方案
- **ip_block_mask**：我贴了 dmesg 的 IP discovery 日志，Claude 直接对照出了检测顺序和位掩码的不匹配
- **GART 翻译**：Claude 从 gem5 源码中追踪了 `getGARTAddr()` 的乘 8 变换，发现了 VRAM 地址被误导入 GART 路径的问题
- **Q35 内存拆分**：Claude 翻出了 `qemu/hw/i386/pc_q35.c:161` 的硬编码 2GiB 边界，和 gem5 的 3GiB 默认值做对比

整个过程中，15 个 blocking bug 被逐一解决。每个 bug 的修复都建立在对底层系统行为的准确理解上——不是试错，而是溯源。

---

## 07
记忆：跨会话的知识延续

这个项目的开发跨越了多个对话会话——Claude Code 的上下文窗口是有限的，一个超长的调试会话用完上下文后需要续上新的对话。这时候一个关键问题出现了：新的对话怎么知道之前做了什么、哪些 bug 已经修了、哪些还在处理中？

答案是 Claude 的 auto memory 系统。在 `~/.claude/projects/` 目录下，Claude 会自动维护一组记忆文件，记录跨会话的关键信息。这个项目的记忆文件有三个：

1. **MEMORY.md**（主记忆，43 行）：项目结构、gem5 运行环境配置（Docker 镜像名、构建参数、Python 版本）、DRM Client -13 崩溃的修复记录、联合仿真的总体状态
2. **cosim-details.md**（架构细节，69 行）：完整的 BAR 布局、8 个关键修复的摘要、gem5/QEMU 启动命令、GART 页表的精确参数（ptBase、fbBase、PTE 格式）
3. **cosim-debugging.md**（调试进展，63 行）：每个 bug 的文件位置、根因、修复状态（包括"部分修复"这种中间状态）、当前阻塞项

这些记忆文件在实际开发中发挥了几个关键作用：

**避免重复诊断**。当一个新会话开始时，Claude 不需要重新分析整个代码库来理解项目状态。记忆文件里记着"SIGIO 死锁已修复、ip_block_mask 改成了 0x67、GART 回退已实现"，可以直接从上次停下的地方继续。

**保持环境一致性**。gem5 必须在特定的 Docker 镜像里构建和运行（`ghcr.io/gem5/gpu-fs:latest`），QEMU 的串口参数不能和 `-nographic` 混用，磁盘镜像需要用 packer 加特定参数构建——这些环境细节散落在不同的会话中，但被记忆文件统一收集。新会话不会因为用错 Docker 镜像或构建参数而浪费时间。

**追踪增量进展**。调试过程不是线性的。GART 翻译的修复经历了"部分修复"到"完全修复"的过程——记忆文件忠实地记录了这个中间状态，避免了在新会话中误以为问题已经完全解决而跳过验证。

**跨代码库的关联索引**。记忆文件中记录了关键文件的路径（`mi300x_gem5_cosim.cc`、`amdgpu_vm.cc`、`mi300_cosim.py`）、关键常量（`ptBase=0x3EE600000`、`fbBase=0x8000000000`）和关键公式（`getGARTAddr()` 的乘 8 变换）。这些信息分散在三个不同的代码库中，记忆系统将它们集中在一起，形成了一个高效的关联索引。

如果说 Claude 在单次对话中的价值是"快速定位根因"，那么记忆系统的价值就是**让这种能力跨会话延续**。没有记忆系统，每次续上新对话都需要花 10-15 分钟重新建立上下文；有了记忆系统，新会话在几秒钟内就能回到之前的工作状态。

---

## 08
成果：两天交付了什么

| 指标 | 数据 |
|------|------|
| 开发耗时 | ~24 小时（3月6日 06:30 → 3月8日 06:00） |
| 新增代码 | ~2500 行（gem5 C++ ~800，QEMU C ~700，Python 配置 ~200，Shell 脚本 ~800） |
| 解决的 blocking bug | 15 个 |
| 技术文档 | 6 篇（中英双语，共 ~2000 行） |
| Git 提交 | 16 笔（cosim 主仓库） |
| MMIO 操作 | 65,000+ 次无崩溃 |
| HIP 计算测试 | PASSED |
| vfio-user 迁移 | 完成（3月9日），vector_add / transpose / gemm 全部 PASSED |

最终的系统支持：

- **完整 amdgpu 驱动加载**：DRM 初始化，7 个 XCP 分区，gfx942 架构
- **ROCm 工具链**：rocm-smi、rocminfo 正常工作
- **HIP GPU 计算**：hipMalloc、kernel dispatch、hipDeviceSynchronize
- **MSI-X 中断转发**：gem5 → QEMU 事件通知
- **共享内存 DMA**：零拷贝 VRAM + Guest RAM
- **vfio-user 后端**：标准协议，无需自定义 QEMU 代码
- **一键启动**：`./scripts/cosim_launch.sh`

---

## 08.5
vfio-user 迁移：从自定义协议到行业标准

在初始版本使用自定义 socket 协议验证了端到端可行性之后，我们在 3 月 9 日将 QEMU-gem5 通信迁移到了标准的 vfio-user 协议。

vfio-user 是一个用于将 PCI 设备暴露到远程进程的标准协议（QEMU 10.0+ 内置了 `vfio-user-pci` 客户端）。gem5 侧使用 Nutanix 的 libvfio-user 库作为服务端。这意味着：

- **无需自定义 QEMU 代码**：任何支持 vfio-user 的 QEMU 都能直接连接 gem5
- **协议标准化**：BAR 映射、配置空间、中断、DMA 全部由 vfio-user 规范定义
- **更简单的部署**：不再需要维护 QEMU 分支

迁移过程中解决了几个关键问题：
- libvfio-user 的 BAR size 字段是 `uint32_t`，无法表示 16GB VRAM → 改为 `uint64_t`
- 64 位 BAR 的上半部分在 size probing 时需要返回 size mask 的高 32 位
- PCIe Express 和 MSI-X capability 必须在 `vfu_realize_ctx()` 之前注册
- SDMA ring test 超时：`sdma_delay=1e9` 导致 ~500ms 墙钟延迟 → 减小到 1000

迁移后的测试结果：vector_add (120ms)、transpose (6.5s)、gemm (4.7s) 全部 PASSED。

---

## 09
意义：十万块的 GPU 触手可及

MI300X 是 AMD 最强的数据中心 GPU，单卡价格超过 10 万人民币，普通开发者根本摸不到。但通过 QEMU + gem5 联合仿真，你可以在任何一台 x86 Linux 机器上：

- 跑完整的 ROCm 7.0 软件栈
- 编译和运行 HIP 程序
- 在 cycle-accurate 的 GPU 模型上做性能分析
- 调试 amdgpu 驱动的初始化流程
- 开发和验证 GPU 架构的新特性

所有代码已开源：[github.com/zevorn/cosim-gpu](https://github.com/zevorn/cosim-gpu)

```bash
git clone --recurse-submodules git@github.com:zevorn/cosim-gpu.git
cd cosim-gpu
GEM5_BUILD_IMAGE=ghcr.io/gem5/gpu-fs:latest ./scripts/run_mi300x_fs.sh build-all
cd scripts && docker build -t gem5-run:local -f Dockerfile.run . && cd ..
./scripts/cosim_launch.sh
```

---

## 10
后记：放大器，不是替代品

有人可能会说："两天写完的代码能靠谱吗？"

说实话，如果没有 Claude，这个项目至少需要两周。不是因为代码量大——2500 行代码对于一个 PCIe 设备桥接来说并不多——而是因为调试过程中需要同时理解三个系统的内部行为：QEMU 的 Q35 内存布局、gem5 的事件驱动 I/O 模型、Linux amdgpu 驱动的 IP block 初始化顺序。任何一个环节理解错了，就是几小时的调试黑洞。

Claude 的价值不在于帮我写代码，而在于**大幅缩短了从"看到症状"到"理解根因"的时间**。当我贴上一段 dmesg 输出，Claude 能在几秒钟内关联到 gem5 源码中的具体函数和 QEMU 的硬编码常量——这种跨代码库的关联分析，是人工翻源码做不到的速度。

当然，Claude 也不是万能的。所有的测试都是我跑的，所有的架构决策都是我做的（比如选择两条 socket 连接而不是一条，选择 StubWorkload 而不是全系统启动），所有的最终验证都需要在真实环境里确认。AI 是放大器，不是替代品。

但这个放大器确实很强。两天，一个人，一个 AI，十万块的 GPU 搬进了 QEMU。

---

## 参考资料

**项目与源码**

- [cosim-gpu](https://github.com/zevorn/cosim-gpu) — 本项目仓库（含 gem5、QEMU、gem5-resources 子模块）
- [完整使用指南](cosim-usage-guide.md) — 从编译到运行 HIP 测试的全流程
- [技术笔记](cosim-technical-notes.md) — 架构设计、踩坑记录、修复方案
- [MI300X 内存管理](mi300x-memory-management.md) — GART、地址翻译、内存映射
- [Guest GPU 初始化流程](cosim-guest-gpu-init.md) — 驱动加载与设备初始化
- [调试踩坑记录](cosim-debugging-pitfalls.md) — 常见问题与解决方案

**上游项目**

- [gem5](https://www.gem5.org/) — 模块化计算机体系结构仿真器
- [QEMU](https://www.qemu.org/) — 开源机器模拟器与虚拟化工具
- [ROCm](https://rocm.docs.amd.com/) — AMD 开源 GPU 计算平台
- [AMD Instinct MI300X](https://www.amd.com/en/products/accelerators/instinct/mi300/mi300x.html) — 产品规格
- [libvfio-user](https://github.com/nutanix/libvfio-user) — vfio-user 协议服务端库

**开发工具**

- [Claude Code](https://docs.anthropic.com/en/docs/claude-code) — Anthropic 的 CLI 编程助手

---

*泽文，2026 年 3 月*
