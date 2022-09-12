#ifndef VIDEOSOCKET_H
#define VIDEOSOCKET_H

#include <QTcpSocket>

class VideoSocket : public QTcpSocket
{
    Q_OBJECT
public:
    explicit VideoSocket(QObject *parent = nullptr);
    virtual ~VideoSocket();

    qint32 subThreadRecvData(quint8 *buf, qint32 bufSize);
};

#endif // VIDEOSOCKET_H
