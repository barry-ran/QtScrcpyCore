#include <QDebug>

#include "bufferutil.h"
#include "controlmsg.h"

#define MIN(X, Y) ((X) < (Y) ? (X) : (Y))
#define MAX(X, Y) ((X) > (Y) ? (X) : (Y))
#define CLAMP(V, X, Y) MIN(MAX((V), (X)), (Y))

ControlMsg::ControlMsg(ControlMsgType controlMsgType) : QScrcpyEvent(Control)
{
    m_data.type = controlMsgType;
}

ControlMsg::~ControlMsg()
{
    if (CMT_SET_CLIPBOARD == m_data.type && Q_NULLPTR != m_data.setClipboard.text) {
        delete m_data.setClipboard.text;
        m_data.setClipboard.text = Q_NULLPTR;
    } else if (CMT_INJECT_TEXT == m_data.type && Q_NULLPTR != m_data.injectText.text) {
        delete m_data.injectText.text;
        m_data.injectText.text = Q_NULLPTR;
    }
}

void ControlMsg::setInjectKeycodeMsgData(AndroidKeyeventAction action, AndroidKeycode keycode, quint32 repeat, AndroidMetastate metastate)
{
    m_data.injectKeycode.action = action;
    m_data.injectKeycode.keycode = keycode;
    m_data.injectKeycode.repeat = repeat;
    m_data.injectKeycode.metastate = metastate;
}

void ControlMsg::setInjectTextMsgData(QString &text)
{
    // write length (2 byte) + string (non nul-terminated)
    if (CONTROL_MSG_INJECT_TEXT_MAX_LENGTH < text.length()) {
        // injecting a text takes time, so limit the text length
        text = text.left(CONTROL_MSG_INJECT_TEXT_MAX_LENGTH);
    }
    QByteArray tmp = text.toUtf8();
    m_data.injectText.text = new char[tmp.length() + 1];
    memcpy(m_data.injectText.text, tmp.data(), tmp.length());
    m_data.injectText.text[tmp.length()] = '\0';
}

void ControlMsg::setInjectTouchMsgData(
    quint64 id,
    AndroidMotioneventAction action,
    AndroidMotioneventButtons actionButtons,
    AndroidMotioneventButtons buttons,
    QRect position,
    float pressure)
{
    m_data.injectTouch.id = id;
    m_data.injectTouch.action = action;
    m_data.injectTouch.actionButtons = actionButtons;
    m_data.injectTouch.buttons = buttons;
    m_data.injectTouch.position = position;
    m_data.injectTouch.pressure = pressure;
}

void ControlMsg::setInjectScrollMsgData(QRect position, qint32 hScroll, qint32 vScroll, AndroidMotioneventButtons buttons)
{
    m_data.injectScroll.position = position;
    m_data.injectScroll.hScroll = CLAMP(hScroll, -1, 1);
    m_data.injectScroll.vScroll = CLAMP(vScroll, -1, 1);
    m_data.injectScroll.buttons = buttons;
}

void ControlMsg::setGetClipboardMsgData(ControlMsg::GetClipboardCopyKey copyKey) 
{
    m_data.getClipboard.copyKey = copyKey;
}

void ControlMsg::setSetClipboardMsgData(QString &text, bool paste)
{
    if (text.isEmpty()) {
        m_data.setClipboard.text = Q_NULLPTR;
        return;
    }
    if (CONTROL_MSG_CLIPBOARD_TEXT_MAX_LENGTH < text.length()) {
        text = text.left(CONTROL_MSG_CLIPBOARD_TEXT_MAX_LENGTH);
    }

    QByteArray tmp = text.toUtf8();
    m_data.setClipboard.text = new char[tmp.length() + 1];
    memcpy(m_data.setClipboard.text, tmp.data(), tmp.length());
    m_data.setClipboard.text[tmp.length()] = '\0';
    m_data.setClipboard.paste = paste;
    m_data.setClipboard.sequence = 0;
}

void ControlMsg::setDisplayPowerData(bool on)
{
    m_data.setDisplayPower.on = on;
}

void ControlMsg::setBackOrScreenOnData(bool down)
{
    m_data.backOrScreenOn.action = down ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP;
}

void ControlMsg::writePosition(QBuffer &buffer, const QRect &value)
{
    BufferUtil::write32(buffer, value.left());
    BufferUtil::write32(buffer, value.top());
    BufferUtil::write16(buffer, value.width());
    BufferUtil::write16(buffer, value.height());
}

quint16 ControlMsg::flostToU16fp(float f)
{
    Q_ASSERT(f >= 0.0f && f <= 1.0f);
    quint32 u = f * 0x1p16f; // 2^16
    if (u >= 0xffff) {
        u = 0xffff;
    }
    return (quint16)u;
}

qint16 ControlMsg::flostToI16fp(float f)
{
    Q_ASSERT(f >= -1.0f && f <= 1.0f);
    qint32 i = f * 0x1p15f; // 2^15
    Q_ASSERT(i >= -0x8000);
    if (i >= 0x7fff) {
        Q_ASSERT(i == 0x8000); // for f == 1.0f
        i = 0x7fff;
    }
    return (qint16)i;
}

QByteArray ControlMsg::serializeData()
{
    QByteArray byteArray;
    QBuffer buffer(&byteArray);
    buffer.open(QBuffer::WriteOnly);
    buffer.putChar(m_data.type);

    switch (m_data.type) {
    case CMT_INJECT_KEYCODE:
        buffer.putChar(m_data.injectKeycode.action);
        BufferUtil::write32(buffer, m_data.injectKeycode.keycode);
        BufferUtil::write32(buffer, m_data.injectKeycode.repeat);
        BufferUtil::write32(buffer, m_data.injectKeycode.metastate);
        break;
    case CMT_INJECT_TEXT:
        BufferUtil::write32(buffer, static_cast<quint32>(strlen(m_data.injectText.text)));
        buffer.write(m_data.injectText.text, strlen(m_data.injectText.text));
        break;
    case CMT_INJECT_TOUCH: {
        buffer.putChar(m_data.injectTouch.action);
        BufferUtil::write64(buffer, m_data.injectTouch.id);
        writePosition(buffer, m_data.injectTouch.position);
        quint16 pressure = flostToU16fp(m_data.injectTouch.pressure);
        BufferUtil::write16(buffer, pressure);
        BufferUtil::write32(buffer, m_data.injectTouch.actionButtons);
        BufferUtil::write32(buffer, m_data.injectTouch.buttons);
    } break;
    case CMT_INJECT_SCROLL:
        writePosition(buffer, m_data.injectScroll.position);
        BufferUtil::write16(buffer, flostToI16fp(m_data.injectScroll.hScroll));
        BufferUtil::write16(buffer, flostToI16fp(m_data.injectScroll.vScroll));
        BufferUtil::write32(buffer, m_data.injectScroll.buttons);
        break;
    case CMT_BACK_OR_SCREEN_ON:
        buffer.putChar(m_data.backOrScreenOn.action);
        break;
    case CMT_GET_CLIPBOARD:
        buffer.putChar(m_data.getClipboard.copyKey);
        break;
    case CMT_SET_CLIPBOARD:
        BufferUtil::write64(buffer, m_data.setClipboard.sequence);
        buffer.putChar(!!m_data.setClipboard.paste);
        if (m_data.setClipboard.text != Q_NULLPTR) {
            BufferUtil::write32(buffer, static_cast<quint32>(strlen(m_data.setClipboard.text)));
            buffer.write(m_data.setClipboard.text, strlen(m_data.setClipboard.text));
        } else {
            BufferUtil::write32(buffer, 0);
            buffer.write(m_data.setClipboard.text, 0);
        }
        break;
    case CMT_SET_DISPLAY_POWER:
        buffer.putChar(m_data.setDisplayPower.on);
        break;
    case CMT_EXPAND_NOTIFICATION_PANEL:
    case CMT_EXPAND_SETTINGS_PANEL:
    case CMT_COLLAPSE_PANELS:
    case CMT_ROTATE_DEVICE:
        break;
    default:
        qDebug() << "Unknown event type:" << m_data.type;
        break;
    }
    buffer.close();
    return byteArray;
}
