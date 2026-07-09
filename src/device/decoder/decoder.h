#ifndef DECODER_H
#define DECODER_H
#include <QObject>
#include <QMutex>

extern "C"
{
#include "libavcodec/avcodec.h"
}

#include <functional>

class VideoBuffer;
class VTDecoder;

enum DecodeMode {
    MODE_FFMPEG = 0,
    MODE_VT_METAL = 1
};

class Decoder : public QObject
{
    Q_OBJECT
public:
    Decoder(std::function<void(int width, int height, uint8_t* dataY, uint8_t* dataU, uint8_t* dataV, int linesizeY, int linesizeU, int linesizeV)> onFrame, QObject *parent = Q_NULLPTR);
    virtual ~Decoder();

    bool open(int decodeMode = MODE_FFMPEG);
    void close();
    bool push(const AVPacket *packet);
    void peekFrame(std::function<void(int width, int height, uint8_t* dataRGB32)> onFrame);

    // Metal 渲染回调（主线程执行，CVPixelBufferRef 已 Retain）
    std::function<void(void* cvPixelBuffer, int width, int height)> onMetalFrame = nullptr;

    DecodeMode decodeMode() const { return m_decodeMode; }

signals:
    void updateFPS(quint32 fps);

private slots:
    void onNewFrame();

signals:
    void newFrame();

private:
    void pushFrame();
    bool pushVT(const AVPacket *packet);

    // FFmpeg 软解
    VideoBuffer *m_vb = Q_NULLPTR;
    AVCodecContext *m_codecCtx = Q_NULLPTR;
    bool m_isCodecCtxOpen = false;
    std::function<void(int,int,uint8_t*,uint8_t*,uint8_t*,int,int,int)> m_onFrame = Q_NULLPTR;

    // Metal 硬解
    VTDecoder *m_vtDecoder = Q_NULLPTR;
    DecodeMode m_decodeMode = MODE_FFMPEG;
    QMutex m_pixelBufferMutex;
    void *m_lastPixelBuffer = nullptr;
    int m_lastFrameWidth = 0;
    int m_lastFrameHeight = 0;
};

#endif // DECODER_H
