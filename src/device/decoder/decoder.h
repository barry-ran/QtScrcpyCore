#ifndef DECODER_H
#define DECODER_H
#include <QObject>

extern "C"
{
#include "libavcodec/avcodec.h"
}

#include <functional>

#include "IDecoder.h"

class VideoBuffer;
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
    void onVideoSessionChanged(const QSize &size) override;
    void setRenderExpiredFrames(bool enabled) override;

private slots:
    void onNewFrame();

signals:
    void newFrame();

private:
    void pushFrame();

private:
    VideoBuffer *m_vb = Q_NULLPTR;
    AVCodecContext *m_codecCtx = Q_NULLPTR;
    bool m_isCodecCtxOpen = false;
    std::function<void(int, int, uint8_t*, uint8_t*, uint8_t*, int, int, int)> m_onFrame = Q_NULLPTR;
};

#endif // DECODER_H
