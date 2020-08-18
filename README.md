# 文档说明
本项目基于公版的[AmebaZ2 SDK](https://github.com/ambiot/ambz2_sdk.git)集成移植[IoT Explorer C-SDK](https://github.com/tencentyun/qcloud-iot-explorer-sdk-embedded-c.git) ，并提供可运行的demo，同时介绍了在代码级别如何使用WiFi配网API，可结合腾讯连连小程序进行softAP及simpleConfig方式WiFi配网及设备绑定。

> 公版的**AmebaZ2 SDK**是针对RTL8720cf WiFi芯片的裁剪版本的SDK，NDA版本的SDK需要咨询Realtek获取，对于其他系列的WiFi芯片对应的SDK（8710/8720D等）都可以参照本项目的集成移植**IoT Explorer C-SDK**。

### 1. 开发准备
- 硬件准备
准备开发板 AmebaZ2 Dev Board Version DEV_2V0
[![AmebaZ2-Dev-Board](https://www.amebaiot.com/wp-content/uploads/2019/07/start-1.png)][amebaZ2-guide-link]

-  代码下载
```
git clone https://github.com/yougaliu007/ambz2_sdk.git
```

- 修改设备三元组信息
在`component/common/application/tencent_iot_explorer/platform/HAL_Device_freertos.c`中修改在腾讯云物联网开发平台注册的设备信息（目前仅支持密钥设备）：

```
/* Product Id */
static char sg_product_id[MAX_SIZE_OF_PRODUCT_ID + 1]    = "PRODUCT_ID";
/* Device Name */
static char sg_device_name[MAX_SIZE_OF_DEVICE_NAME + 1]  = "YOUR_DEV_NAME";
/* Device Secret */
static char sg_device_secret[MAX_SIZE_OF_DEVICE_SECRET + 1] = "YOUR_IOT_PSK";
```

### 2. 编译
- AmebaZ2 SDK支持IAR、Cygwin、GCC多种交叉编译方式，具体参阅doc目录文档`AN0500 Realtek Ameba-ZII application note.en.pdf`，本文档基于Ubuntu环境的GCC编译方式描述。

- Ubuntu默认启动为`dash`，编译过程的脚本依赖`bash`，会导致脚本解析错误，所以需要[切换为`bash`](https://blog.csdn.net/gatieme/article/details/52136411)

- 进入到`project/realtek_amebaz2_v0_example/GCC-RELEASE`目录，执行编译
```
$ cd project/realtek_amebaz2_v0_example/GCC-RELEASE
$ make clean && make 
```

### 3. 烧写
- 下载方法，USB连接CON3口，按住开发板右侧uart_download按键，再按左侧复位键，进入升级模式。
- 双击 `tools/flash/AmebaZII_PGTool_v1.2.16/AmebaZII_PGTool_v1.2.16.exe`，选择`project/realtek_amebaz2_v0_example/GCC-RELEASE/application_is/Debug/bin`目录下的`flash_is.bin`，点击`Downlaod`
 ![down](https://main.qcloudimg.com/raw/b8b9765a94980349ef57e32f0727e029.png)

更多编译及下载的指导参考Realtek官网指引
[官网 SDK](https://github.com/ambiot/ambz2_sdk.git)
[官网入门](https://www.amebaiot.com/cn/amazon-freertos-getting-started/)

### 4. WiFi配网说明
工程里面包含了WiFi配网及设备绑定的代码，关于softAP配网协议及接口使用请看 [WiFi设备softAP配网](https://github.com/tencentyun/qcloud-iot-esp-wifi/blob/master/docs/WiFi%E8%AE%BE%E5%A4%87softAP%E9%85%8D%E7%BD%91v2.0.md)，关于simpleConfig配网协议及接口使用请看 [WiFi设备simpleConfig配网](https://github.com/tencentyun/qcloud-iot-esp-wifi/blob/master/docs/WiFi%E8%AE%BE%E5%A4%87SmartConfig%E9%85%8D%E7%BD%91.md)。

demo入口 `qcloud_demo_task` 示例了`softAP`和`simpleConfig`两种配网方式的选择，`WIFI_PROV_SOFT_AP_ENABLE`配置是否使能softAP配网，`WIFI_PROV_SIMPLE_CONFIG_ENABLE`配置是否使能simpleConfig。


### 5. 更新腾讯云物联 C-SDK
如果有需要更新SDK，可根据使用的平台按下面步骤下载更新：

- 从GitHub下载C-SDK代码

```
# 腾讯云物联网开发平台 IoT Explorer
git clone https://github.com/tencentyun/qcloud-iot-explorer-sdk-embedded-c.git

# 腾讯云物联网通信 IoT Hub
git clone https://github.com/tencentyun/qcloud-iot-sdk-embedded-c.git
```
- 配置CMake并执行代码抽取
在C-SDK根目录的 CMakeLists.txt 中配置为freertos平台，并开启代码抽取功能。其他配置选项可以根据需要修改：
```
set(BUILD_TYPE                  "release")
set(PLATFORM 	                "freertos")
set(EXTRACT_SRC ON)
set(FEATURE_AT_TCP_ENABLED OFF)
```
Linux环境运行以下命令
```
mkdir build
cd build
cmake ..
```
即可在output/qcloud_iot_c_sdk中找到相关代码文件。

- 拷贝替换项文件
将output/qcloud_iot_c_sdk 文件夹拷贝替换本项目目录的 `component/common/application/tencent_iot_explorer` 文件夹
- qcloud_iot_c_sdk 目录介绍：
`include`目录为SDK供用户使用的API及可变参数，其中config.h为根据编译选项生成的编译宏。API具体介绍请参考C-SDK文档**C-SDK_API及可变参数说明**。
`platform`目录为平台相关的代码，可根据设备的具体情况进行修改适配。具体的函数说明请参考C-SDK文档**C-SDK_Porting跨平台移植概述**
`sdk_src`为SDK的核心逻辑及协议相关代码，一般不需要修改，其中`internal_inc`为SDK内部使用的头文件。