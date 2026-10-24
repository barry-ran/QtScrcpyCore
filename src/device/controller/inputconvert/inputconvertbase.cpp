#include "inputconvertbase.h"
#include "controller.h"

InputConvertBase::InputConvertBase(Controller *controller) : QObject(controller), m_controller(controller)
{
    Q_ASSERT(controller);
}

InputConvertBase::~InputConvertBase() {}

void InputConvertBase::sendControlMsg(ControlMsg *msg)
{
    if (!msg) {
        return;
    }
    if (!m_controller) {
        delete msg;
        return;
    }
    if (msg->type() == ControlMsg::CMT_INJECT_TOUCH) {
        const auto &touch = msg->m_data.injectTouch;
        const bool active = m_activeTouches.contains(touch.id);
        if ((touch.action == AMOTION_EVENT_ACTION_DOWN && active)
                || (touch.action != AMOTION_EVENT_ACTION_DOWN && !active)) {
            delete msg;
            return;
        }
        if (touch.action == AMOTION_EVENT_ACTION_UP) {
            m_activeTouches.remove(touch.id);
        } else {
            m_activeTouches.insert(touch.id, touch.position);
        }
    } else if (msg->type() == ControlMsg::CMT_INJECT_KEYCODE) {
        const auto &key = msg->m_data.injectKeycode;
        if (key.action == AKEY_EVENT_ACTION_DOWN) {
            m_activeKeys.insert(static_cast<int>(key.keycode));
        } else if (key.action == AKEY_EVENT_ACTION_UP
                   && !m_activeKeys.remove(static_cast<int>(key.keycode))) {
            delete msg;
            return;
        }
    }
    m_controller->postControlMsg(msg);
}

void InputConvertBase::resetInput(const QSize &frameSize)
{
    const auto touches = m_activeTouches;
    for (auto it = touches.constBegin(); it != touches.constEnd(); ++it) {
        QRect position = it.value();
        // A new video session may already use different dimensions on the server.
        if (frameSize.isValid() && !frameSize.isEmpty() && !position.size().isEmpty()) {
            const QPoint point(qRound(qreal(position.x()) * frameSize.width() / position.width()),
                               qRound(qreal(position.y()) * frameSize.height() / position.height()));
            position = QRect(point, frameSize);
        }
        auto *release = new ControlMsg(ControlMsg::CMT_INJECT_TOUCH);
        release->setInjectTouchMsgData(it.key(), AMOTION_EVENT_ACTION_UP,
                                      static_cast<AndroidMotioneventButtons>(0),
                                      static_cast<AndroidMotioneventButtons>(0), position, 0.0f);
        sendControlMsg(release);
    }
    const auto keys = m_activeKeys;
    for (int key : keys) {
        auto *release = new ControlMsg(ControlMsg::CMT_INJECT_KEYCODE);
        release->setInjectKeycodeMsgData(AKEY_EVENT_ACTION_UP, static_cast<AndroidKeycode>(key), 0, AMETA_NONE);
        sendControlMsg(release);
    }
    m_repeat = 0;
}
