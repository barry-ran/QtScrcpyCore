#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QHostAddress>
#include <QThread>
#include <QTimer>
#include <QTimerEvent>

#include "server.h"

#define DEVICE_NAME_FIELD_LENGTH 64
#define VIDEO_META_LENGTH 16
#define SOCKET_NAME_PREFIX "scrcpy"
#define MAX_CONNECT_COUNT 30
#define MAX_RESTART_COUNT 1

static quint32 bufferRead32be(const quint8 *buf)
{
    return static_cast<quint32>((buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3]);
}

Server::Server(QObject *parent) : QObject(parent)
{
    connect(&m_workProcess, &qsc::AdbProcess::adbProcessResult, this, &Server::onWorkProcessResult);
    connect(&m_serverProcess, &qsc::AdbProcess::adbProcessResult, this, &Server::onWorkProcessResult);

    connect(&m_serverSocket, &QTcpServer::newConnection, this, [this]() {
        QTcpSocket *tmp = m_serverSocket.nextPendingConnection();
        if (!tmp) {
            return;
        }
        if (dynamic_cast<VideoSocket *>(tmp)) {
            auto *videoSocket = static_cast<VideoSocket *>(tmp);
            if (m_videoSocket) {
                videoSocket->deleteLater();
                return;
            }
            m_videoSocket = videoSocket;
            connect(videoSocket, &QTcpSocket::readyRead, this, [this]() {
                processReverseVideoInfo();
            });
            connect(videoSocket, &QTcpSocket::disconnected, this, [this]() {
                if (!m_connectionEstablished && m_serverStartStep == SSS_RUNNING) {
                    retryServerAfterConnectionFailure();
                }
            });
            processReverseVideoInfo();
        } else {
            if (m_controlSocket) {
                tmp->deleteLater();
                return;
            }
            m_controlSocket = tmp;
            connect(m_controlSocket, &QTcpSocket::disconnected, this, [this]() {
                if (!m_connectionEstablished && m_serverStartStep == SSS_RUNNING) {
                    retryServerAfterConnectionFailure();
                }
            });
            completeReverseConnection();
        }
    });
}

Server::~Server() {}

bool Server::pushServer()
{
    if (m_workProcess.isRuning()) {
        m_workProcess.kill();
    }
    m_workProcess.push(m_params.serial, m_params.serverLocalPath, m_params.serverRemotePath);
    return true;
}

bool Server::enableTunnelReverse()
{
    if (m_workProcess.isRuning()) {
        m_workProcess.kill();
    }
    m_workProcess.reverse(m_params.serial, QString(SOCKET_NAME_PREFIX "_%1").arg(m_params.scid, 8, 16, QChar('0')), m_params.localPort);
    return true;
}

bool Server::disableTunnelReverse()
{
    qsc::AdbProcess *adb = new qsc::AdbProcess();
    if (!adb) {
        return false;
    }
    connect(adb, &qsc::AdbProcess::adbProcessResult, this, [this](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
            sender()->deleteLater();
        }
    });
    adb->reverseRemove(m_params.serial, QString(SOCKET_NAME_PREFIX "_%1").arg(m_params.scid, 8, 16, QChar('0')));
    return true;
}

bool Server::enableTunnelForward()
{
    if (m_workProcess.isRuning()) {
        m_workProcess.kill();
    }
    m_workProcess.forward(m_params.serial, m_params.localPort, QString(SOCKET_NAME_PREFIX "_%1").arg(m_params.scid, 8, 16, QChar('0')));
    return true;
}
bool Server::disableTunnelForward()
{
    qsc::AdbProcess *adb = new qsc::AdbProcess();
    if (!adb) {
        return false;
    }
    connect(adb, &qsc::AdbProcess::adbProcessResult, this, [this](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
            sender()->deleteLater();
        }
    });
    adb->forwardRemove(m_params.serial, m_params.localPort);
    return true;
}

bool Server::execute()
{
    if (m_serverProcess.isRuning()) {
        m_serverProcess.kill();
    }
    QStringList args;
    args << "shell";
    args << QString("CLASSPATH=%1").arg(m_params.serverRemotePath);
    args << "app_process";

#ifdef SERVER_DEBUGGER
#define SERVER_DEBUGGER_PORT "5005"

    args <<
#ifdef SERVER_DEBUGGER_METHOD_NEW
        /* Android 9 and above */
        "-XjdwpProvider:internal -XjdwpOptions:transport=dt_socket,suspend=y,server=y,address="
#else
        /* Android 8 and below */
        "-agentlib:jdwp=transport=dt_socket,suspend=y,server=y,address="
#endif
        SERVER_DEBUGGER_PORT,
#endif

        args << "/"; // unused;
    args << "com.genymobile.scrcpy.Server";
    args << m_params.serverVersion;

    args << QString("video_bit_rate=%1").arg(QString::number(m_params.bitRate));
    const bool cameraMode = m_params.videoSource == qsc::VIDEO_SOURCE_CAMERA;
    if (cameraMode) {
        args << "video_source=camera";
        if (!m_params.cameraId.isEmpty()) {
            args << QString("camera_id=%1").arg(m_params.cameraId);
        } else {
            args << QString("camera_facing=%1")
                        .arg(m_params.cameraFacing == qsc::CAMERA_FACING_FRONT ? "front" : "back");
        }
    }
    if (!m_params.logLevel.isEmpty()) {
        args << QString("log_level=%1").arg(m_params.logLevel);
    }
    if (m_params.maxSize > 0) {
        args << QString("max_size=%1").arg(QString::number(m_params.maxSize));
    }
    if (m_params.maxFps > 0) {
        args << QString("max_fps=%1").arg(QString::number(m_params.maxFps));
    }

    // capture_orientation=@90
    // 有@表示锁定，没@不锁定
    // 有值表示指定方向，没值表示原始方向
    if (1 == m_params.captureOrientationLock) {
        args << QString("capture_orientation=@%1").arg(m_params.captureOrientation);
    } else if (2 == m_params.captureOrientationLock) {
        args << QString("capture_orientation=@");
    } else {
        args << QString("capture_orientation=%1").arg(m_params.captureOrientation);
    }
    if (m_tunnelForward) {
        args << QString("tunnel_forward=true");
    }
    if (!cameraMode) {
        if (!m_params.crop.isEmpty()) {
            args << QString("crop=%1").arg(m_params.crop);
        }
        if (!m_params.newDisplay.isEmpty()) {
            args << QString("new_display=%1").arg(m_params.newDisplay);
            if (m_params.flexDisplay) {
                args << "flex_display=true";
            }
            if (!m_params.vdDestroyContent) {
                args << "vd_destroy_content=false";
            }
            if (!m_params.vdSystemDecorations) {
                args << "vd_system_decorations=false";
            }
            if (!m_params.displayImePolicy.isEmpty()) {
                args << QString("display_ime_policy=%1").arg(m_params.displayImePolicy);
            }
        } else if (m_params.displayId > 0) {
            args << QString("display_id=%1").arg(m_params.displayId);
        }
    }
    if (!m_params.control) {
        args << QString("control=false");
    }
    // 默认是0，不需要设置
    // args << "display_id=0";
    // 默认是false，不需要设置
    // args << "show_touches=false";
    if (!cameraMode) {
        if (m_params.stayAwake) {
            args << QString("stay_awake=true");
        }
        if (m_params.keepActive) {
            args << "keep_active=true";
        }
    }
    // code option
    // https://github.com/Genymobile/scrcpy/commit/080a4ee3654a9b7e96c8ffe37474b5c21c02852a
    // <https://d.android.com/reference/android/media/MediaFormat>
    if (!m_params.codecOptions.isEmpty()) {
        args << QString("video_codec_options=%1").arg(m_params.codecOptions);
    }
    if (!m_params.codecName.isEmpty()) {
        args << QString("video_encoder=%1").arg(m_params.codecName);
    }
    args << "audio=false";
    // 服务端默认-1，可不传
    if (-1 != m_params.scid) {
        args << QString("scid=%1").arg(m_params.scid, 8, 16, QChar('0'));
    }

    // 默认是false，不需要设置
    // args << "power_off_on_close=false";

    // 下面的参数都用服务端默认值即可，尽量减少参数传递，传参太长导致三星手机报错：stack corruption detected (-fstack-protector)
    /*
    args << "clipboard_autosync=true";    
    args << "downsize_on_error=true";
    args << "cleanup=true";
    args << "power_on=true";
    
    args << "send_device_meta=true";
    args << "send_frame_meta=true";
    args << "send_dummy_byte=true";
    args << "raw_video_stream=false";
    */

#ifdef SERVER_DEBUGGER
    qInfo("Server debugger waiting for a client on device port " SERVER_DEBUGGER_PORT "...");
    // From the computer, run
    //     adb forward tcp:5005 tcp:5005
    // Then, from Android Studio: Run > Debug > Edit configurations...
    // On the left, click on '+', "Remote", with:
    //     Host: localhost
    //     Port: 5005
    // Then click on "Debug"
#endif

    // adb -s P7C0218510000537 shell CLASSPATH=/data/local/tmp/scrcpy-server app_process / com.genymobile.scrcpy.Server 0 8000000 false
    // mark: crop input format: "width:height:x:y" or "" for no crop, for example: "100:200:0:0"
    // 这条adb命令是阻塞运行的，m_serverProcess进程不会退出了
    m_serverProcess.execute(m_params.serial, args);
    return true;
}

bool Server::start(Server::ServerParams params)
{
    m_params = params;
    stopAcceptTimeoutTimer();
    stopConnectTimeoutTimer();
    stopConnectAttemptTimeoutTimer();
    m_connectAttemptInProgress = false;
    m_forwardVideoDummyRead = false;
    m_forwardControlDummyRead = false;
    m_connectionEstablished = false;
    m_videoInfoReady = false;
    m_deviceName.clear();
    m_deviceSize = QSize();
    m_serverSocket.close();
    m_serverSocket.resetIncomingConnectionOrder();
    if (m_params.useReverse) {
        m_serverSocket.setMaxPendingConnections(2);
        if (!m_serverSocket.listen(QHostAddress::LocalHost, m_params.localPort)) {
            const quint16 requestedPort = m_params.localPort;
            if (!m_serverSocket.listen(QHostAddress::LocalHost, 0)) {
                qCritical() << "Could not open a local reverse-tunnel listener:" << m_serverSocket.errorString();
                m_serverStartStep = SSS_NULL;
                emit serverStarted(false);
                return false;
            }
            m_params.localPort = m_serverSocket.serverPort();
            qWarning() << "Reverse-tunnel port" << requestedPort << "was busy; using" << m_params.localPort;
        }
    }
    m_serverStartStep = SSS_PUSH;
    return startServerByStep();
}

bool Server::connectTo()
{
    if (SSS_RUNNING != m_serverStartStep) {
        qWarning("server not run");
        return false;
    }

    if (!m_tunnelForward && !m_videoSocket) {
        startAcceptTimeoutTimer();
        return true;
    }

    startConnectTimeoutTimer();
    return true;
}

bool Server::isReverse()
{
    return !m_tunnelForward;
}

Server::ServerParams Server::getParams()
{
    return m_params;
}

void Server::timerEvent(QTimerEvent *event)
{
    if (event && m_acceptTimeoutTimer == event->timerId()) {
        stopAcceptTimeoutTimer();
        qWarning("timed out waiting for the server's reverse tunnel connections");
        retryServerAfterConnectionFailure();
    } else if (event && m_connectTimeoutTimer == event->timerId()) {
        onConnectTimer();
    } else if (event && m_connectAttemptTimeoutTimer == event->timerId()) {
        stopConnectAttemptTimeoutTimer();
        failForwardConnectAttempt();
    }
}

VideoSocket* Server::removeVideoSocket()
{
    VideoSocket* socket = m_videoSocket;
    m_videoSocket = Q_NULLPTR;
    return socket;
}

QTcpSocket *Server::getControlSocket()
{
    return m_controlSocket;
}

void Server::stop()
{
    stopAcceptTimeoutTimer();
    stopConnectTimeoutTimer();
    stopConnectAttemptTimeoutTimer();
    m_connectAttemptInProgress = false;
    m_connectionEstablished = false;

    if (m_controlSocket) {
        m_controlSocket->abort();
        m_controlSocket->deleteLater();
        m_controlSocket = nullptr;
    }
    if (m_videoSocket) {
        m_videoSocket->abort();
        m_videoSocket->deleteLater();
        m_videoSocket = nullptr;
    }
    // ignore failure
    m_serverStartStep = SSS_NULL;
    m_serverProcess.kill();
    if (m_tunnelEnabled) {
        if (m_tunnelForward) {
            disableTunnelForward();
        } else {
            disableTunnelReverse();
        }
        m_tunnelForward = false;
        m_tunnelEnabled = false;
    }
    m_serverSocket.close();
}

bool Server::startServerByStep()
{
    bool stepSuccess = false;
    // push, enable tunnel et start the server
    if (SSS_NULL != m_serverStartStep) {
        switch (m_serverStartStep) {
        case SSS_PUSH:
            stepSuccess = pushServer();
            break;
        case SSS_ENABLE_TUNNEL_REVERSE:
            stepSuccess = enableTunnelReverse();
            break;
        case SSS_ENABLE_TUNNEL_FORWARD:
            stepSuccess = enableTunnelForward();
            break;
        case SSS_EXECUTE_SERVER:
            // server will connect to our server socket
            stepSuccess = execute();
            break;
        default:
            break;
        }
    }

    if (!stepSuccess) {
        emit serverStarted(false);
    }
    return stepSuccess;
}

Server::DeviceInfoReadResult Server::readInfo(VideoSocket *videoSocket,
                                               QString &deviceName,
                                               QSize &size)
{
    if (!videoSocket || !videoSocket->isValid()) {
        return DeviceInfoReadResult::Invalid;
    }
    constexpr qint64 headerSize = DEVICE_NAME_FIELD_LENGTH + VIDEO_META_LENGTH;
    if (videoSocket->bytesAvailable() < headerSize) {
        return DeviceInfoReadResult::Incomplete;
    }

    const QByteArray header = videoSocket->read(headerSize);
    if (header.size() != headerSize) {
        return DeviceInfoReadResult::Incomplete;
    }
    QByteArray deviceNameBytes = header.left(DEVICE_NAME_FIELD_LENGTH);
    deviceNameBytes[DEVICE_NAME_FIELD_LENGTH - 1] = '\0'; // guard against a missing terminator
    deviceName = QString::fromUtf8(deviceNameBytes.constData());

    // scrcpy 4.x: codec id (4 bytes), then session metadata (flags, width, height).
    const auto *metadata = reinterpret_cast<const quint8 *>(header.constData() + DEVICE_NAME_FIELD_LENGTH);
    size.setWidth(static_cast<int>(bufferRead32be(metadata + 8)));
    size.setHeight(static_cast<int>(bufferRead32be(metadata + 12)));

    if (size.width() <= 0 || size.height() <= 0) {
        return DeviceInfoReadResult::Invalid;
    }
    return DeviceInfoReadResult::Complete;
}

void Server::startAcceptTimeoutTimer()
{
    stopAcceptTimeoutTimer();
    m_acceptTimeoutTimer = startTimer(10000);
}

void Server::stopAcceptTimeoutTimer()
{
    if (m_acceptTimeoutTimer) {
        killTimer(m_acceptTimeoutTimer);
        m_acceptTimeoutTimer = 0;
    }
}

void Server::startConnectTimeoutTimer()
{
    stopConnectTimeoutTimer();
    m_connectTimeoutTimer = startTimer(300);
}

void Server::stopConnectTimeoutTimer()
{
    if (m_connectTimeoutTimer) {
        killTimer(m_connectTimeoutTimer);
        m_connectTimeoutTimer = 0;
    }
    m_connectCount = 0;
}

void Server::stopConnectAttemptTimeoutTimer()
{
    if (m_connectAttemptTimeoutTimer) {
        killTimer(m_connectAttemptTimeoutTimer);
        m_connectAttemptTimeoutTimer = 0;
    }
}

void Server::onConnectTimer()
{
    if (m_connectAttemptInProgress || m_connectionEstablished) {
        return;
    }

    m_connectAttemptInProgress = true;
    m_forwardVideoDummyRead = false;
    m_forwardControlDummyRead = false;
    m_videoInfoReady = false;
    m_videoSocket = new VideoSocket(this);
    m_controlSocket = new QTcpSocket(this);

    connect(m_videoSocket, &QTcpSocket::readyRead, this, [this]() {
        processForwardVideoInfo();
    });
    connect(m_controlSocket, &QTcpSocket::readyRead, this, [this]() {
        processForwardControlData();
    });
    QPointer<QTcpSocket> controlSocket = m_controlSocket;
    connect(controlSocket, &QTcpSocket::connected, this, [controlSocket]() {
        if (controlSocket) {
            controlSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        }
    });
    const auto failed = [this](QAbstractSocket::SocketError) {
        failForwardConnectAttempt();
    };
    connect(m_videoSocket, &QAbstractSocket::errorOccurred, this, failed);
    connect(m_controlSocket, &QAbstractSocket::errorOccurred, this, failed);
    connect(m_videoSocket, &QTcpSocket::disconnected, this, [this]() {
        failForwardConnectAttempt();
    });
    connect(m_controlSocket, &QTcpSocket::disconnected, this, [this]() {
        failForwardConnectAttempt();
    });

    m_connectAttemptTimeoutTimer = startTimer(3000);
    m_videoSocket->connectToHost(QHostAddress::LocalHost, m_params.localPort);
    m_controlSocket->connectToHost(QHostAddress::LocalHost, m_params.localPort);
}

void Server::processReverseVideoInfo()
{
    if (m_connectionEstablished || !m_videoSocket) {
        return;
    }

    QString deviceName;
    QSize deviceSize;
    const DeviceInfoReadResult result = readInfo(m_videoSocket, deviceName, deviceSize);
    if (result == DeviceInfoReadResult::Incomplete) {
        return;
    }
    if (result == DeviceInfoReadResult::Invalid) {
        qWarning("could not read the device video header from the reverse tunnel");
        retryServerAfterConnectionFailure();
        return;
    }

    m_deviceName = deviceName;
    m_deviceSize = deviceSize;
    m_videoInfoReady = true;
    completeReverseConnection();
}

void Server::processForwardVideoInfo()
{
    if (!m_connectAttemptInProgress || !m_videoSocket) {
        return;
    }

    if (!m_forwardVideoDummyRead) {
        if (m_videoSocket->bytesAvailable() < 1) {
            return;
        }
        m_videoSocket->read(1);
        m_forwardVideoDummyRead = true;
    }

    if (!m_videoInfoReady) {
        QString deviceName;
        QSize deviceSize;
        const DeviceInfoReadResult result = readInfo(m_videoSocket, deviceName, deviceSize);
        if (result == DeviceInfoReadResult::Incomplete) {
            return;
        }
        if (result == DeviceInfoReadResult::Invalid) {
            failForwardConnectAttempt();
            return;
        }
        m_deviceName = deviceName;
        m_deviceSize = deviceSize;
        m_videoInfoReady = true;
    }

    completeForwardConnection();
}

void Server::processForwardControlData()
{
    if (!m_connectAttemptInProgress || !m_controlSocket || m_forwardControlDummyRead) {
        return;
    }
    if (m_controlSocket->bytesAvailable() < 1) {
        return;
    }

    m_controlSocket->read(1);
    m_forwardControlDummyRead = true;
    completeForwardConnection();
}

void Server::completeReverseConnection()
{
    if (m_connectionEstablished || !m_videoSocket || !m_controlSocket || !m_videoInfoReady) {
        return;
    }

    m_connectionEstablished = true;
    stopAcceptTimeoutTimer();
    QObject::disconnect(m_videoSocket.data(), nullptr, this, nullptr);
    QObject::disconnect(m_controlSocket.data(), nullptr, this, nullptr);
    m_serverSocket.close();
    disableTunnelReverse();
    m_tunnelEnabled = false;
    m_restartCount = 0;
    emit serverStarted(true, m_deviceName, m_deviceSize);
}

void Server::completeForwardConnection()
{
    if (!m_connectAttemptInProgress || !m_videoInfoReady || !m_forwardControlDummyRead) {
        return;
    }

    m_connectAttemptInProgress = false;
    m_connectionEstablished = true;
    stopConnectAttemptTimeoutTimer();
    stopConnectTimeoutTimer();
    QObject::disconnect(m_videoSocket.data(), nullptr, this, nullptr);
    QObject::disconnect(m_controlSocket.data(), nullptr, this, nullptr);
    disableTunnelForward();
    m_tunnelEnabled = false;
    m_restartCount = 0;
    emit serverStarted(true, m_deviceName, m_deviceSize);
}

void Server::failForwardConnectAttempt()
{
    if (!m_connectAttemptInProgress) {
        return;
    }

    m_connectAttemptInProgress = false;
    stopConnectAttemptTimeoutTimer();
    if (m_videoSocket) {
        QObject::disconnect(m_videoSocket.data(), nullptr, this, nullptr);
        m_videoSocket->abort();
        m_videoSocket->deleteLater();
        m_videoSocket = nullptr;
    }
    if (m_controlSocket) {
        QObject::disconnect(m_controlSocket.data(), nullptr, this, nullptr);
        m_controlSocket->abort();
        m_controlSocket->deleteLater();
        m_controlSocket = nullptr;
    }
    m_forwardVideoDummyRead = false;
    m_forwardControlDummyRead = false;
    m_videoInfoReady = false;

    if (++m_connectCount >= MAX_CONNECT_COUNT) {
        qWarning("timed out connecting to the server through the forward tunnel");
        retryServerAfterConnectionFailure();
    }
}

void Server::retryServerAfterConnectionFailure()
{
    if (m_serverStartStep == SSS_NULL) {
        return;
    }

    const ServerParams params = m_params;
    stop();
    if (m_restartCount < MAX_RESTART_COUNT) {
        ++m_restartCount;
        qWarning("server connection failed; restarting the server once");
        start(params);
        return;
    }

    m_restartCount = 0;
    emit serverStarted(false);
}

void Server::onWorkProcessResult(qsc::AdbProcess::ADB_EXEC_RESULT processResult)
{
    if (sender() == &m_workProcess) {
        if (SSS_NULL != m_serverStartStep) {
            switch (m_serverStartStep) {
            case SSS_PUSH:
                if (qsc::AdbProcess::AER_SUCCESS_EXEC == processResult) {
                    if (m_params.useReverse) {
                        m_serverStartStep = SSS_ENABLE_TUNNEL_REVERSE;
                    } else {
                        m_tunnelForward = true;
                        m_serverStartStep = SSS_ENABLE_TUNNEL_FORWARD;
                    }
                    startServerByStep();
                } else if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
                    qCritical("adb push failed");
                    m_serverStartStep = SSS_NULL;
                    emit serverStarted(false);
                }
                break;
            case SSS_ENABLE_TUNNEL_REVERSE:
                if (qsc::AdbProcess::AER_SUCCESS_EXEC == processResult) {
                    // At the application level, the device part is "the server" because it
                    // serves video stream and control. However, at the network level, the
                    // client listens and the server connects to the client. That way, the
                    // client can listen before starting the server app, so there is no need to
                    // try to connect until the server socket is listening on the device.
                    m_serverSocket.setMaxPendingConnections(2);
                    if (!m_serverSocket.isListening()
                            && !m_serverSocket.listen(QHostAddress::LocalHost, m_params.localPort)) {
                        qCritical() << QString("Could not listen on port %1").arg(m_params.localPort).toStdString().c_str();
                        m_serverStartStep = SSS_NULL;
                        disableTunnelReverse();
                        emit serverStarted(false);
                        break;
                    }

                    m_serverStartStep = SSS_EXECUTE_SERVER;
                    startServerByStep();
                } else if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
                    // 有一些设备reverse会报错more than o'ne device，adb的bug
                    // https://github.com/Genymobile/scrcpy/issues/5
                    qCritical("adb reverse failed");
                    m_serverSocket.close();
                    m_serverSocket.resetIncomingConnectionOrder();
                    m_tunnelForward = true;
                    m_serverStartStep = SSS_ENABLE_TUNNEL_FORWARD;
                    startServerByStep();
                }
                break;
            case SSS_ENABLE_TUNNEL_FORWARD:
                if (qsc::AdbProcess::AER_SUCCESS_EXEC == processResult) {
                    m_serverStartStep = SSS_EXECUTE_SERVER;
                    startServerByStep();
                } else if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
                    qCritical("adb forward failed");
                    m_serverStartStep = SSS_NULL;
                    emit serverStarted(false);
                }
                break;
            default:
                break;
            }
        }
    }
    if (sender() == &m_serverProcess) {
        if (SSS_EXECUTE_SERVER == m_serverStartStep) {
            if (qsc::AdbProcess::AER_SUCCESS_START == processResult) {
                m_serverStartStep = SSS_RUNNING;
                m_tunnelEnabled = true;
                connectTo();
            } else if (qsc::AdbProcess::AER_ERROR_START == processResult) {
                if (!m_tunnelForward) {
                    m_serverSocket.close();
                    disableTunnelReverse();
                } else {
                    disableTunnelForward();
                }
                qCritical("adb shell start server failed");
                m_serverStartStep = SSS_NULL;
                emit serverStarted(false);
            }
        } else if (SSS_RUNNING == m_serverStartStep) {
            if (!m_connectionEstablished) {
                qWarning("scrcpy server exited before the device connection was established");
                retryServerAfterConnectionFailure();
            } else {
                m_serverStartStep = SSS_NULL;
                emit serverStoped();
            }
        }
    }
}
