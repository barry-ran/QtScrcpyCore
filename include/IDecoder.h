#pragma once
#include <QObject>
#include <QSize>
#include <functional>

extern "C"
{
#include "libavcodec/avcodec.h"
}

/// 解码器抽象接口
/// Decoder (FFmpeg) 和 VTDecoder (VideoToolbox) 均实现此接口，
/// 由 Device 根据 decodeMode 工厂创建具体实例。
class IDecoder : public QObject
{
    Q_OBJECT
public:
    explicit IDecoder(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~IDecoder() = default;

    /// 打开解码器，初始化资源
    virtual bool open() = 0;

    /// 关闭解码器，释放所有资源
    virtual void close() = 0;

    /// 推送一帧待解码的 AVPacket
    virtual bool push(const AVPacket *packet) = 0;

    /// 截屏：获取当前帧的 RGB32 数据
    virtual void peekFrame(std::function<void(int width, int height, uint8_t* dataRGB32)> onFrame) = 0;
    /// A new scrcpy video session starts after encoder/display reconfiguration.
    virtual void onVideoSessionChanged(const QSize &size) { Q_UNUSED(size); }
    virtual void setRenderExpiredFrames(bool enabled) { Q_UNUSED(enabled); }

signals:
    void updateFPS(quint32 fps);
};
