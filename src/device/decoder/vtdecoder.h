#ifndef VTDECODER_H
#define VTDECODER_H

#include <QObject>

// 前向声明 CVPixelBufferRef（在 .mm 中 include CoreVideo）
typedef struct __CVBuffer *CVPixelBufferRef;

class VTDecoder : public QObject
{
    Q_OBJECT
public:
    explicit VTDecoder(QObject *parent = nullptr);
    virtual ~VTDecoder();

    // 运行时检测：仅在 macOS arm64 返回 true
    static bool isAvailable();

    // 初始化解码器（首次 decode 时懒初始化 VTDecompressionSession）
    bool open();
    void close();

    // 解码一帧 H.264 数据
    // data: H.264 NAL 单元数据（首次调用包含 AVCDecoderConfigurationRecord 前缀）
    // size: 数据大小
    // pts: 毫秒时间戳
    // outPixelBuffer: 输出 CVPixelBufferRef（NV12, Metal 兼容, IOSurface 支持）— 调用方负责 CVPixelBufferRelease
    // outWidth/outHeight: 输出帧尺寸
    // 返回 true 表示成功解码
    bool decode(const unsigned char *data, int size, qint64 pts,
                CVPixelBufferRef &outPixelBuffer, int &outWidth, int &outHeight);

    int currentWidth() const { return m_currentWidth; }
    int currentHeight() const { return m_currentHeight; }

    // 内部实现（在 .mm 中定义，供 VT 回调等静态函数使用）
    struct Impl;

signals:
    void updateFPS(quint32 fps);

private:
    Impl *d = nullptr;

    int m_currentWidth = 0;
    int m_currentHeight = 0;
};

#endif // VTDECODER_H
