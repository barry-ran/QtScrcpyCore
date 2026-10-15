#ifndef INPUTCONVERT_H
#define INPUTCONVERT_H

#include "inputconvertbase.h"

class InputConvertNormal : public InputConvertBase
{
    Q_OBJECT
public:
    InputConvertNormal(Controller *controller);
    virtual ~InputConvertNormal();

    virtual void mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize);
    virtual void wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize);
    virtual void keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize);

private:
    AndroidMotioneventButtons convertMouseButtons(Qt::MouseButtons buttonState);
    AndroidMotioneventButtons convertMouseButton(Qt::MouseButton button);
    AndroidKeycode convertKeyCode(int key, Qt::KeyboardModifiers modifiers);
    AndroidMetastate convertMetastate(Qt::KeyboardModifiers modifiers);

    // pinch-to-zoom / tilt simulation (ported from scrcpy)
    // Hold Ctrl and drag the left button: a "virtual finger" is injected at
    // the point symmetric to the dragging finger with respect to the center
    // of the screen, which Android apps interpret as a two-finger pinch.
    //   Ctrl        -> pinch-to-zoom / rotate (both axes inverted)
    //   Shift       -> vertical tilt (x inverted only)
    //   Ctrl+Shift  -> horizontal tilt (y inverted only)
    void sendVirtualFingerEvent(AndroidMotioneventAction action, const QPoint &pos,
                                const QSize &frameSize);

    bool m_vfingerDown = false;
    bool m_vfingerInvertX = false;
    bool m_vfingerInvertY = false;
};

#endif // INPUTCONVERT_H
