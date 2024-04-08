#ifndef STREAM_H
#define STREAM_H

#include <QPointer>
#include <QSize>
#include <QThread>

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}

class VideoSocket;
class Demuxer : public QThread
{
    Q_OBJECT
public:
    Demuxer(QObject *parent = Q_NULLPTR);
    virtual ~Demuxer();

public:
    static bool init();
    static void deInit();

    void installVideoSocket(VideoSocket* videoSocket);
    void setFrameSize(const QSize &frameSize);
    bool startDecode();
    void stopDecode();

signals:
    void onStreamStop();
    void getFrame(AVPacket* packet);
    void getConfigFrame(AVPacket* packet);

protected:
    void run();
    bool recvPacket(AVPacket *packet);
    bool pushPacket(AVPacket *packet);
    bool processConfigPacket(AVPacket *packet);
    bool parse(AVPacket *packet);
    bool processFrame(AVPacket *packet);
    qint32 recvData(quint8 *buf, qint32 bufSize);

private:
    QPointer<VideoSocket> m_videoSocket;
    QSize m_frameSize;

    AVCodecContext *m_codecCtx = Q_NULLPTR;
    AVCodecParserContext *m_parser = Q_NULLPTR;
    // successive packets may need to be concatenated, until a non-config
    // packet is available
    AVPacket* m_pending = Q_NULLPTR;
};

#endif // STREAM_H
