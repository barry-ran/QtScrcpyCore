#ifndef VTDECODER_H
#define VTDECODER_H

#include <QObject>
#include <QMutex>
#include <QAtomicInt>
#include <functional>

#include "IDecoder.h"

/// VideoToolbox 硬件解码器，实现 IDecoder 接口
/// 仅 macOS arm64 可用，由 Device 根据 decodeMode 工厂创建
class VTDecoder : public IDecoder
{
    Q_OBJECT
public:
    explicit VTDecoder(QObject *parent = nullptr);
    virtual ~VTDecoder();

    // 运行时检测：仅在 macOS arm64 返回 true
    static bool isAvailable();

    // IDecoder 接口
    bool open() override;
    void close() override;
    bool push(const AVPacket *packet) override;
    void peekFrame(std::function<void(int,int,uint8_t*)> onFrame) override;

    // Metal 渲染回调（主线程执行，CVPixelBufferRef 已 Retain）
    // 由 Device 在创建后设置
    std::function<void(void* cvPixelBuffer, int width, int height)> onFrame = nullptr;

    // 解码一帧 H.264 Annex B 数据（内部方法，由 push 调用，在 .mm 中实现）
    // data: H.264 NAL 单元数据
    // size: 数据大小
    // pts: 毫秒时间戳
    // outPixelBuffer: 输出 CVPixelBufferRef（NV12, Metal 兼容, IOSurface 支持）— 调用方负责 CVPixelBufferRelease
    // outWidth/outHeight: 输出帧尺寸
    // 返回 true 表示成功解码
    bool decode(const unsigned char *data, int size, qint64 pts,
                void *&outPixelBuffer, int &outWidth, int &outHeight);

    int currentWidth() const { return m_currentWidth; }
    int currentHeight() const { return m_currentHeight; }

    // 内部实现（在 .mm 中定义，供 VT 回调等静态函数使用）
    struct Impl;

private slots:
    void onNewFrame();

signals:
    void newFrame();

private:
    Impl *d = nullptr;

    int m_currentWidth = 0;
    int m_currentHeight = 0;

    // 背压：pixel buffer 互斥 + 主线程帧数控制
    QMutex m_pixelBufferMutex;
    void *m_lastPixelBuffer = nullptr;
    int m_lastFrameWidth = 0;
    int m_lastFrameHeight = 0;
    QAtomicInt m_frameInFlight{0};
};

#endif // VTDECODER_H
