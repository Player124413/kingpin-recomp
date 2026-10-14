// Gamepads on macOS and iOS: the GameController framework (Xbox, PlayStation, Switch Pro, MFi)
// reported to the portable gamepad layer (win32/gamepad.cpp).
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include "../win32/hle.h"
#include "platform.h"

namespace
{
    void readController(GCExtendedGamepad* g)
    {
        pad::State p;
        p.lx = g.leftThumbstick.xAxis.value;
        p.ly = g.leftThumbstick.yAxis.value;
        p.rx = g.rightThumbstick.xAxis.value;
        p.ry = g.rightThumbstick.yAxis.value;
        p.lt = g.leftTrigger.value;
        p.rt = g.rightTrigger.value;
        const bool bits[] = { g.buttonA.pressed, g.buttonB.pressed, g.buttonX.pressed, g.buttonY.pressed,
                              g.leftShoulder.pressed, g.rightShoulder.pressed, g.leftTrigger.value > 0.4f,
                              g.rightTrigger.value > 0.4f, g.leftThumbstickButton.pressed,
                              g.rightThumbstickButton.pressed, g.buttonOptions.pressed };
        for (int k = 0; k < 11; k++)
            if (bits[k])
                p.buttons |= 1u << k;
        p.up = g.dpad.up.pressed;
        p.down = g.dpad.down.pressed;
        p.left = g.dpad.left.pressed;
        p.right = g.dpad.right.pressed;
        p.start = g.buttonMenu.pressed;
        pad::update(p);
    }

    void attach(GCController* c)
    {
        if (!c.extendedGamepad)
            return;
        logf("[pad] controller connected: %s", c.vendorName.UTF8String ?: "gamepad");
        c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad* g, GCControllerElement*) { readController(g); };
        readController(c.extendedGamepad);
    }
}

namespace platform
{
    void gamepadStart()
    {
        dispatch_async(dispatch_get_main_queue(), ^{
            GCController.shouldMonitorBackgroundEvents = YES;
            for (GCController* c in GCController.controllers)
                attach(c);
            [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidConnectNotification object:nil queue:nil
                                                        usingBlock:^(NSNotification* n) { attach(n.object); }];
            [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidDisconnectNotification object:nil queue:nil
                                                        usingBlock:^(NSNotification*) {
                                                            logf("[pad] controller disconnected");
                                                            pad::disconnected();
                                                        }];
        });
    }
}
