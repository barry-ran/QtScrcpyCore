#pragma once
#include <QString>

/// 解码模式枚举
enum DecodeMode {
    MODE_FFMPEG = 0,
    MODE_VT_METAL = 1
};

namespace qsc {

enum VideoSource {
    VIDEO_SOURCE_DISPLAY = 0,
    VIDEO_SOURCE_CAMERA,
};

enum CameraFacing {
    CAMERA_FACING_BACK = 0,
    CAMERA_FACING_FRONT,
};

struct DeviceParams {
    // necessary
    QString serial = "";              // 设备序列号
    QString serverLocalPath = "";     // 本地安卓server路径

    // optional
    QString serverRemotePath = "/data/local/tmp/scrcpy-server.jar";    // 要推送到远端设备的server路径
    quint16 localPort = 27183;        // reverse时本地监听端口
    quint16 maxSize = 720;            // 视频分辨率
    quint32 bitRate = 2000000;        // 视频比特率
    quint32 maxFps = 0;               // 视频最大帧率
    VideoSource videoSource = VIDEO_SOURCE_DISPLAY;
    CameraFacing cameraFacing = CAMERA_FACING_BACK;
    QString cameraId = "";            // 指定相机 ID，空值时按 cameraFacing 选择
    bool useReverse = true;           // true:先使用adb reverse，失败后自动使用adb forward；false:直接使用adb forward
    int captureOrientationLock = 0;   // 是否锁定采集方向 0不锁定 1锁定指定方向 2锁定原始方向
    int captureOrientation = 0;       // 采集方向 0 90 180 270
    bool stayAwake = false;           // 是否保持唤醒
    QString serverVersion = "4.1";    // server版本
    QString logLevel = "debug";     // log级别 verbose/debug/info/warn/error
    // 编码选项 ""表示默认
    // 例如 CodecOptions="profile=1,level=2"
    // 更多编码选项参考 https://d.android.com/reference/android/media/MediaFormat
    QString codecOptions = "";
    // 指定编码器名称(必须是H.264编码器)，""表示默认
    // 例如 CodecName="OMX.qcom.video.encoder.avc"
    QString codecName = "";
    quint32 scid = -1; // 随机数，作为localsocket名字后缀，方便同时连接同一个设备多次
    QString crop = "";                // WxH:X:Y，flex 模式下必须为空
    qint32 displayId = 0;             // 已有 Android display，newDisplay 非空时忽略
    QString newDisplay = "";          // WxH[/dpi]；空值表示不创建虚拟显示
    bool flexDisplay = false;         // 仅 newDisplay + video + control
    bool vdDestroyContent = true;
    bool vdSystemDecorations = true;
    QString displayImePolicy = "";    // local/fallback/hide，空值采用 server 默认值
    bool keepActive = false;
    QString startApp = "";            // 建连后交由 control 协议发送

    QString recordPath = "";          // 视频保存路径
    QString recordFileFormat = "mp4"; // 视频保存格式 mp4/mkv
    bool recordFile = false;          // 录制到文件

    QString pushFilePath = "/sdcard/"; // 推送到安卓设备的文件保存路径（必须以/结尾）

    bool closeScreen = false;         // 启动时自动息屏
    bool display = true;              // 是否显示画面（或者仅仅后台录制）
    bool renderExpiredFrames = false; // 是否渲染延迟视频帧
    QString gameScript = "";          // 游戏映射脚本
    int decodeMode = 0;               // 0=FFmpeg OpenGL (默认), 1=VideoToolbox Metal (Apple Silicon)
};
    
}
