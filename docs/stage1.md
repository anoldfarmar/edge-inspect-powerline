# 阶段1：阅读与使用

先读 `apps/infer.cpp` 的 `main()`，再看 `include/edgeinspect/inference_engine.h` 和 `src/inference_engine.cpp`。CLI流程为：参数校验 → 图片/RGB预处理 → 初始化引擎 → 一次推理 → JSON保存。

顶层 `CMakeLists.txt` 中：

```cmake
add_executable(edgeinspect_cli apps/infer.cpp)
target_link_libraries(edgeinspect_cli PRIVATE edgeinspect_engine)
```

这明确了源码与程序的对应关系。`cmake -S . -B artifacts/build` 生成规则，`cmake --build artifacts/build` 才执行编译和链接。`edgeinspect_cli` 是二进制，阅读源码应打开 `.cpp`。

## 数据与资源

`PreparedInput` 拥有RGB字节，`Result` 拥有结果，`unique_ptr<Impl>` 隐藏SDK实现。引擎禁止复制和移动，约定同一时刻一个线程调用；构造时加载指定模型和五类正常库，重复推理不反复加载。

`Context` 最先声明、最后析构，即使构造函数在模型初始化后加载库失败也会释放ctx。每次调用的 `OutputLease` 管理RKNN输出：成功获取后才拥有缓冲，正常或异常退出都先释放输出，再销毁ctx。返回结果不借用SDK裸指针，诊断张量显式复制到自己的vector。

这对应C语言中 `init → 使用 → cleanup`，C++通过构造/析构及作用域自动执行清理。析构不抛异常；释放错误记录或传播，不无限重试，也不承诺用线程超时终止卡死的RKNN调用。

## 冻结推理口径

- 输入320×320、RGB、stretch；普通API提交uint8 NHWC、pass_through=0，不再次除255。
- 一次前向同时输出 `[1,18,2100]` predictions 和 `[1,64,40,40]` raw P3。
- `want_float=1` 获取逻辑FP32输出；native NC1HWC2只记录属性，不直接传给NCHW后处理。
- 沿用conf=0.25、NMS IoU=0.7、max_det=300，已知资产选最高分框，无框保留整图fallback。
- 原 `insplad::postprocess_p3`：40×40 RoIAlign，scale=0.125、aligned=true、sampling_ratio=1，再patch通道L2 eps=1e-12。
- 精确欧氏1NN，每个patch开根，最大8个距离均值。正常库不是RKNN模型本身的一部分。

C++预处理使用OpenCV解码（忽略EXIF）及Pillow11.3兼容的bilinear两遍22位舍入；来源和许可证见 `third_party/notices/`。支持8位JPEG及非调色板8位PNG，其他模式明确拒绝。已经测试的RGB一致不等于所有解码器、所有图片永远等价。

## 诊断与错误

`--rgb` 读取307200字节精确RGB；`--rgb-out` 保存图片入口的实际RGB；`--diagnostics` 保存真实predictions、P3、ROI及归一化patch。默认只保留小型结果。

`--max-bytes` 默认32MiB、`--max-pixels` 默认2400万，解码前检查头部尺寸。结果文件用 `O_EXCL` 独占创建，拒绝覆盖；处理短写和EINTR，写入失败移除部分结果文件。不承诺断电持久性。

`steady_clock` 分别记录预处理、模型调用、检测/ROI/L2和检索/top8。`--repeat 100` 准备一次RGB、初始化一次引擎，每轮实际前向；不含解码的重复调用延迟不能称完整图片或摄像头FPS。

本阶段只有CLI，尚无任务ID/队列/数据库/HTTP；普通用户需要NPU设备访问权限和可写输出目录，不能仅因保存失败就改用root。
