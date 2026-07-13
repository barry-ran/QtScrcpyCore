#ifndef DECODER_H
#define DECODER_H
#include <QObject>
#include <functional>

extern "C"
{
#include "libavcodec/avcodec.h"
}

#include "IDecoder.h"

class VideoBuffer;

/// FFmpeg 软件解码器，实现 IDecoder 接口
class Decoder : public IDecoder
{
    Q_OBJECT
public:
    Decoder(std::function<void(int width, int height, uint8_t* dataY, uint8_t* dataU, uint8_t* dataV, int linesizeY, int linesizeU, int linesizeV)> onFrame, QObject *parent = Q_NULLPTR);
    virtual ~Decoder();

    bool open() override;
    void close() override;
    bool push(const AVPacket *packet) override;
    void peekFrame(std::function<void(int width, int height, uint8_t* dataRGB32)> onFrame) override;

private slots:
    void onNewFrame();

signals:
    void newFrame();

private:
    void pushFrame();

    // FFmpeg 软解
    VideoBuffer *m_vb = Q_NULLPTR;
    AVCodecContext *m_codecCtx = Q_NULLPTR;
    bool m_isCodecCtxOpen = false;
    std::function<void(int,int,uint8_t*,uint8_t*,uint8_t*,int,int,int)> m_onFrame = Q_NULLPTR;
};

#endif // DECODER_H
