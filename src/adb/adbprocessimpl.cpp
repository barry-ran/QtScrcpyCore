#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
#include <QRegExp>
#else
#include <QRegularExpression>
#endif

#include "adbprocessimpl.h"

QString AdbProcessImpl::s_adbPath = "";
extern QString g_adbPath;

AdbProcessImpl::AdbProcessImpl(QObject *parent) : QProcess(parent)
{
    initSignals();
}

AdbProcessImpl::~AdbProcessImpl()
{
    if (isRuning()) {
        close();
    }
}

const QString &AdbProcessImpl::getAdbPath()
{
    if (s_adbPath.isEmpty()) {
        QStringList potentialPaths;
        potentialPaths << QString::fromLocal8Bit(qgetenv("QTSCRCPY_ADB_PATH"))
                       << g_adbPath
                       << QCoreApplication::applicationDirPath() + "/adb";

        for (const QString &path : potentialPaths) {
            QFileInfo fileInfo(path);
            if (!path.isEmpty() && fileInfo.isFile()) {
                s_adbPath = path;
                break;
            }
        }

        if (s_adbPath.isEmpty()) {
            // 如果所有路径都不满足条件，可以选择抛出异常或设置默认值
            qWarning() << "ADB路径未找到";
        } else {
            qInfo("adb path: %s", QDir(s_adbPath).absolutePath().toUtf8().data());
        }
    }
    return s_adbPath;
}

void AdbProcessImpl::initSignals()
{
    // aboutToQuit not exit event loop, so deletelater is ok
    //connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &AdbProcessImpl::deleteLater);

    connect(this, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        if (NormalExit == exitStatus && 0 == exitCode) {
            emit adbProcessImplResult(qsc::AdbProcess::AER_SUCCESS_EXEC);
        } else {
            //P7C0218510000537        unauthorized ,手机端此时弹出调试认证，要允许调试
            emit adbProcessImplResult(qsc::AdbProcess::AER_ERROR_EXEC);
        }
        qDebug() << "adb return " << exitCode << "exit status " << exitStatus;
    });

    connect(this, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (QProcess::FailedToStart == error) {
            emit adbProcessImplResult(qsc::AdbProcess::AER_ERROR_MISSING_BINARY);
        } else {
            emit adbProcessImplResult(qsc::AdbProcess::AER_ERROR_START);
            QString err = QString("qprocess start error:%1 %2").arg(program()).arg(arguments().join(" "));
            qCritical() << err.toStdString().c_str();
        }
    });

    connect(this, &QProcess::readyReadStandardError, this, [this]() {
        QString tmp = QString::fromUtf8(readAllStandardError()).trimmed();
        m_errorOutput += tmp;
        qWarning() << QString("AdbProcessImpl::error:%1").arg(tmp).toStdString().data();
    });

    connect(this, &QProcess::readyReadStandardOutput, this, [this]() {
        QString tmp = QString::fromUtf8(readAllStandardOutput()).trimmed();
        m_standardOutput += tmp;
        qInfo() << QString("AdbProcessImpl::out:%1").arg(tmp).toStdString().data();
    });

    connect(this, &QProcess::started, this, [this]() { emit adbProcessImplResult(qsc::AdbProcess::AER_SUCCESS_START); });
}

void AdbProcessImpl::execute(const QString &serial, const QStringList &args)
{
    m_standardOutput = "";
    m_errorOutput = "";
    QStringList adbArgs;
    if (!serial.isEmpty()) {
        adbArgs << "-s" << serial;
    }
    adbArgs << args;
    qDebug() << getAdbPath() << adbArgs.join(" ");
    start(getAdbPath(), adbArgs);
}

bool AdbProcessImpl::isRuning()
{
    if (QProcess::NotRunning == state()) {
        return false;
    } else {
        return true;
    }
}

void AdbProcessImpl::setShowTouchesEnabled(const QString &serial, bool enabled)
{
    QStringList adbArgs;
    adbArgs << "shell"
            << "settings"
            << "put"
            << "system"
            << "show_touches";
    adbArgs << (enabled ? "1" : "0");
    execute(serial, adbArgs);
}

QStringList AdbProcessImpl::getDevicesSerialFromStdOut()
{
    // get devices serial by adb devices
    QStringList serials;
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QRegExp lineExp("\r\n|\n");
    QRegExp tExp("\t");
#else
    QRegularExpression lineExp("\r\n|\n");
    QRegularExpression tExp("\t");
#endif
    QStringList devicesInfoList = m_standardOutput.split(lineExp);
    for (QString deviceInfo : devicesInfoList) {
        QStringList deviceInfos = deviceInfo.split(tExp);
        if (2 == deviceInfos.count() && 0 == deviceInfos[1].compare("device")) {
            serials << deviceInfos[0];
        }
    }
    return serials;
}

QString AdbProcessImpl::getDeviceIPFromStdOut()
{
    QString ip = "";
    QString strIPExp = "inet addr:[\\d.]*";
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QRegExp ipRegExp(strIPExp, Qt::CaseInsensitive);
    if (ipRegExp.indexIn(m_standardOutput) != -1) {
        ip = ipRegExp.cap(0);
        ip = ip.right(ip.size() - 10);
    }
#else
    QRegularExpression ipRegExp(strIPExp, QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch match = ipRegExp.match(m_standardOutput);
    if (match.hasMatch()) {
        ip = match.captured(0);
        ip = ip.right(ip.size() - 10);
    }
#endif

    return ip;
}

QString AdbProcessImpl::getDeviceIPByIpFromStdOut()
{
    QString ip = "";

    QString strIPExp = "wlan0    inet [\\d.]*";
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QRegExp ipRegExp(strIPExp, Qt::CaseInsensitive);
    if (ipRegExp.indexIn(m_standardOutput) != -1) {
        ip = ipRegExp.cap(0);
        ip = ip.right(ip.size() - 14);
    }
#else
    QRegularExpression ipRegExp(strIPExp, QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch match = ipRegExp.match(m_standardOutput);
    if (match.hasMatch()) {
        ip = match.captured(0);
        ip = ip.right(ip.size() - 14);
    }
#endif
    qDebug() << "get ip: " << ip;
    return ip;
}

QString AdbProcessImpl::getStdOut()
{
    return m_standardOutput;
}

QString AdbProcessImpl::getErrorOut()
{
    return m_errorOutput;
}

void AdbProcessImpl::forward(const QString &serial, quint16 localPort, const QString &deviceSocketName)
{
    QStringList adbArgs;
    adbArgs << "forward";
    adbArgs << QString("tcp:%1").arg(localPort);
    adbArgs << QString("localabstract:%1").arg(deviceSocketName);
    execute(serial, adbArgs);
}

void AdbProcessImpl::forwardRemove(const QString &serial, quint16 localPort)
{
    QStringList adbArgs;
    adbArgs << "forward";
    adbArgs << "--remove";
    adbArgs << QString("tcp:%1").arg(localPort);
    execute(serial, adbArgs);
}

void AdbProcessImpl::reverse(const QString &serial, const QString &deviceSocketName, quint16 localPort)
{
    QStringList adbArgs;
    adbArgs << "reverse";
    adbArgs << QString("localabstract:%1").arg(deviceSocketName);
    adbArgs << QString("tcp:%1").arg(localPort);
    execute(serial, adbArgs);
}

void AdbProcessImpl::reverseRemove(const QString &serial, const QString &deviceSocketName)
{
    QStringList adbArgs;
    adbArgs << "reverse";
    adbArgs << "--remove";
    adbArgs << QString("localabstract:%1").arg(deviceSocketName);
    execute(serial, adbArgs);
}

void AdbProcessImpl::push(const QString &serial, const QString &local, const QString &remote)
{
    QStringList adbArgs;
    adbArgs << "push";
    adbArgs << local;
    adbArgs << remote;
    execute(serial, adbArgs);
}

void AdbProcessImpl::install(const QString &serial, const QString &local)
{
    QStringList adbArgs;
    adbArgs << "install";
    adbArgs << "-r";
    adbArgs << local;
    execute(serial, adbArgs);
}

void AdbProcessImpl::removePath(const QString &serial, const QString &path)
{
    QStringList adbArgs;
    adbArgs << "shell";
    adbArgs << "rm";
    adbArgs << path;
    execute(serial, adbArgs);
}
