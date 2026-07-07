#import "EngineView.h"

#include "Engine.h"
#include "Input.h"

@implementation EngineView{
    NSMutableAttributedString* _markedText;
    NSTrackingArea* _trackingArea;
    CGPoint _mousePoint;
    BOOL _hasMousePoint;
}

static short int keycodes[256];

static const NSRange _emptyRange = { NSNotFound, 0 };

- (void)viewDidMoveToWindow{
    [[self window] setAcceptsMouseMovedEvents:YES];
}

- (id)initWithCoder:(NSCoder *)coder{
    self = [super initWithCoder:coder];
    
    [self engineInit];
    
    return self;
}

- (id)initWithFrame:(CGRect)frameRect{
    self = [super initWithFrame:frameRect];
    
    [self engineInit];
    
    return self;
}

- (void)engineInit{
    _trackingArea = nil;
    _markedText = [[NSMutableAttributedString alloc] init];
    _mousePoint = CGPointZero;
    _hasMousePoint = NO;
    
    [self createKeyCodeArray];
}

-(void)keyDown:(NSEvent*)event{
    const int key = (int)[self convertKey:[event keyCode]];
    const int mods = (int)[self convertModFlags:[event modifierFlags]];

    doriax::Engine::systemKeyDown(key, [event isARepeat], mods);

    [self interpretKeyEvents:@[event]];
}

-(void)keyUp:(NSEvent*)event{
    const int key = (int)[self convertKey:[event keyCode]];
    const int mods = (int)[self convertModFlags:[event modifierFlags]];
    
    doriax::Engine::systemKeyUp(key, [event isARepeat], mods);
}

- (void)flagsChanged:(NSEvent *)event{
    const unsigned int modifierFlags = [event modifierFlags] & NSEventModifierFlagDeviceIndependentFlagsMask;
    const int key = (int)[self convertKey:[event keyCode]];
    const int mods = (int)[self convertModFlags:[event modifierFlags]];
    const NSUInteger keyFlag = [self convertKeyToModFlag:key];

    if (modifierFlags & keyFlag)
        doriax::Engine::systemKeyDown(key, false, mods);
    else if (modifierFlags | keyFlag)
        doriax::Engine::systemKeyUp(key, false, mods);
 
}

- (void)updateTrackingAreas {
    if(_trackingArea != nil) {
        [self removeTrackingArea:_trackingArea];
        //[trackingArea release];
    }

    int opts = (NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways);
    _trackingArea = [ [NSTrackingArea alloc] initWithRect:[self bounds]
                                                 options:opts
                                                   owner:self
                                                userInfo:nil];
    [self addTrackingArea:_trackingArea];
    [super updateTrackingAreas];
}

-(CGPoint)getMousePoint:(NSEvent*)event{
    const NSPoint pointInView = [event locationInWindow];
    const NSPoint pointInBackingLayer = [self convertPointToBacking:pointInView];
    
    const CGFloat mouseX = pointInBackingLayer.x;
    const CGFloat mouseY = self.drawableSize.height - pointInBackingLayer.y;
    
    return CGPointMake(mouseX, mouseY);
}

-(CGPoint)getTrackedMousePoint:(NSEvent*)event{
    if (!doriax::Engine::isMouseLocked()){
        _mousePoint = [self getMousePoint:event];
        _hasMousePoint = YES;
        return _mousePoint;
    }

    if (!_hasMousePoint){
        _mousePoint = [self getMousePoint:event];
        _hasMousePoint = YES;
        return _mousePoint;
    }

    const NSSize delta = [self convertSizeToBacking:NSMakeSize([event deltaX], [event deltaY])];
    _mousePoint.x += delta.width;
    _mousePoint.y -= delta.height;
    return _mousePoint;
}

- (void)mouseEntered:(NSEvent*)event {
    doriax::Engine::systemMouseEnter();
    if (doriax::Engine::getMouseCursor() != doriax::CursorType::ARROW){
        doriax::Engine::setMouseCursor(doriax::Engine::getMouseCursor());
    }
}

- (void)mouseExited:(NSEvent*)event {
    doriax::Engine::systemMouseLeave();
}

- (void)mouseDown:(NSEvent*)event {
    const int mods = (int)[self convertModFlags:[event modifierFlags]];
    const CGPoint point = [self getTrackedMousePoint:event];
    doriax::Engine::systemMouseDown(D_MOUSE_BUTTON_LEFT, point.x, point.y, mods);
}

- (void)rightMouseDown:(NSEvent*)event {
    const int mods = (int)[self convertModFlags:[event modifierFlags]];
    const CGPoint point = [self getTrackedMousePoint:event];
    doriax::Engine::systemMouseDown(D_MOUSE_BUTTON_RIGHT, point.x, point.y, mods);
}

- (void)otherMouseDown:(NSEvent*)event {
    const int mods = (int)[self convertModFlags:[event modifierFlags]];
    const CGPoint point = [self getTrackedMousePoint:event];
    doriax::Engine::systemMouseDown(D_MOUSE_BUTTON_MIDDLE, point.x, point.y, mods);
}

- (void)mouseUp:(NSEvent*)event {
    const int mods = (int)[self convertModFlags:[event modifierFlags]];
    const CGPoint point = [self getTrackedMousePoint:event];
    doriax::Engine::systemMouseUp(D_MOUSE_BUTTON_LEFT, point.x, point.y, mods);
}

- (void)rightMouseUp:(NSEvent*)event {
    const int mods = (int)[self convertModFlags:[event modifierFlags]];
    const CGPoint point = [self getTrackedMousePoint:event];
    doriax::Engine::systemMouseUp(D_MOUSE_BUTTON_RIGHT, point.x, point.y, mods);
}

- (void)otherMouseUp:(NSEvent*)event {
    const int mods = (int)[self convertModFlags:[event modifierFlags]];
    const CGPoint point = [self getTrackedMousePoint:event];
    doriax::Engine::systemMouseUp(D_MOUSE_BUTTON_MIDDLE, point.x, point.y, mods);
}

- (void)mouseDragged:(NSEvent*)event {
    [self mouseMoved:event];
}

- (void)rightMouseDragged:(NSEvent*)event {
    [self mouseMoved:event];
}

- (void)otherMouseDragged:(NSEvent*)event {
    [self mouseMoved:event];
}

- (void)mouseMoved:(NSEvent*)event {
    const int mods = (int)[self convertModFlags:[event modifierFlags]];
    const CGPoint point = [self getTrackedMousePoint:event];
    doriax::Engine::systemMouseMove(point.x, point.y, mods);
}

- (void)scrollWheel:(NSEvent*)event {
    const int mods = (int)[self convertModFlags:[event modifierFlags]];
    float dx = (float) event.scrollingDeltaX;
    float dy = (float) event.scrollingDeltaY;
    if (event.hasPreciseScrollingDeltas) {
        dx *= 0.1;
        dy *= 0.1;
    }
    if (dx != 0.0f || dy != 0.0f) {
        doriax::Engine::systemMouseScroll(dx, dy, mods);
    }
}

- (BOOL)canBecomeKeyView{
    return YES;
}

- (BOOL)acceptsFirstResponder{
    return YES;
}

- (BOOL)wantsUpdateLayer{
    return YES;
}

- (void)deleteBackward{
    doriax::Engine::systemCharInput('\b');
}

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange{
    NSString* characters;

    if ([string isKindOfClass:[NSAttributedString class]])
        characters = [string string];
    else
        characters = (NSString*) string;

    const NSUInteger length = [characters length];
    for (NSUInteger i = 0;  i < length;  i++) {
        const unichar codepoint = [characters characterAtIndex:i];
        if ((codepoint & 0xFF00) == 0xF700)
            continue;

        doriax::Engine::systemCharInput(codepoint);
    }
}

- (void)doCommandBySelector:(SEL)selector{
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange{
    if ([string isKindOfClass:[NSAttributedString class]])
        _markedText = [[NSMutableAttributedString alloc] initWithAttributedString:string];
    else
        _markedText = [[NSMutableAttributedString alloc] initWithString:string];
}

- (void)unmarkText{
    [[_markedText mutableString] setString:@""];
}

- (NSRange)selectedRange{
    return _emptyRange;
}

- (NSRange)markedRange{
    if ([_markedText length] > 0)
        return NSMakeRange(0, [_markedText length] - 1);
    else
        return _emptyRange;
}

- (BOOL)hasMarkedText{
    //return [_markedText length] > 0;
    return true; // to send all keyboard events (ex: backspace) to insertText
}

- (nullable NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range actualRange:(nullable NSRangePointer)actualRange {
    return nil;
}

- (NSArray*)validAttributesForMarkedText{
    return [NSArray array];
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(nullable NSRangePointer)actualRange{
    return NSMakeRect(self.frame.origin.x, self.frame.origin.y, 0.0, 0.0);
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point{
    return 0;
}

- (NSInteger)convertModFlags:(NSUInteger)flags{
    int mods = 0;

    if (flags & NSEventModifierFlagShift)
        mods |= D_MODIFIER_SHIFT;
    if (flags & NSEventModifierFlagControl)
        mods |= D_MODIFIER_CONTROL;
    if (flags & NSEventModifierFlagOption)
        mods |= D_MODIFIER_ALT;
    if (flags & NSEventModifierFlagCommand)
        mods |= D_MODIFIER_SUPER;
    if (flags & NSEventModifierFlagCapsLock)
        mods |= D_MODIFIER_CAPS_LOCK;

    return mods;
}

- (NSInteger)convertKey:(NSUInteger)key{
    if (key >= sizeof(keycodes) / sizeof(keycodes[0]))
        return D_KEY_UNKNOWN;

    return keycodes[key];
}

- (NSUInteger)convertKeyToModFlag:(NSInteger)key{
    switch (key) {
        case D_KEY_LEFT_SHIFT:
        case D_KEY_RIGHT_SHIFT:
            return NSEventModifierFlagShift;
        case D_KEY_LEFT_CONTROL:
        case D_KEY_RIGHT_CONTROL:
            return NSEventModifierFlagControl;
        case D_KEY_LEFT_ALT:
        case D_KEY_RIGHT_ALT:
            return NSEventModifierFlagOption;
        case D_KEY_LEFT_SUPER:
        case D_KEY_RIGHT_SUPER:
            return NSEventModifierFlagCommand;
        case D_KEY_CAPS_LOCK:
            return NSEventModifierFlagCapsLock;
    }

    return 0;
}

- (void)createKeyCodeArray{
    keycodes[0x1D] = D_KEY_0;
    keycodes[0x12] = D_KEY_1;
    keycodes[0x13] = D_KEY_2;
    keycodes[0x14] = D_KEY_3;
    keycodes[0x15] = D_KEY_4;
    keycodes[0x17] = D_KEY_5;
    keycodes[0x16] = D_KEY_6;
    keycodes[0x1A] = D_KEY_7;
    keycodes[0x1C] = D_KEY_8;
    keycodes[0x19] = D_KEY_9;
    keycodes[0x00] = D_KEY_A;
    keycodes[0x0B] = D_KEY_B;
    keycodes[0x08] = D_KEY_C;
    keycodes[0x02] = D_KEY_D;
    keycodes[0x0E] = D_KEY_E;
    keycodes[0x03] = D_KEY_F;
    keycodes[0x05] = D_KEY_G;
    keycodes[0x04] = D_KEY_H;
    keycodes[0x22] = D_KEY_I;
    keycodes[0x26] = D_KEY_J;
    keycodes[0x28] = D_KEY_K;
    keycodes[0x25] = D_KEY_L;
    keycodes[0x2E] = D_KEY_M;
    keycodes[0x2D] = D_KEY_N;
    keycodes[0x1F] = D_KEY_O;
    keycodes[0x23] = D_KEY_P;
    keycodes[0x0C] = D_KEY_Q;
    keycodes[0x0F] = D_KEY_R;
    keycodes[0x01] = D_KEY_S;
    keycodes[0x11] = D_KEY_T;
    keycodes[0x20] = D_KEY_U;
    keycodes[0x09] = D_KEY_V;
    keycodes[0x0D] = D_KEY_W;
    keycodes[0x07] = D_KEY_X;
    keycodes[0x10] = D_KEY_Y;
    keycodes[0x06] = D_KEY_Z;

    keycodes[0x27] = D_KEY_APOSTROPHE;
    keycodes[0x2A] = D_KEY_BACKSLASH;
    keycodes[0x2B] = D_KEY_COMMA;
    keycodes[0x18] = D_KEY_EQUAL;
    keycodes[0x32] = D_KEY_GRAVE_ACCENT;
    keycodes[0x21] = D_KEY_LEFT_BRACKET;
    keycodes[0x1B] = D_KEY_MINUS;
    keycodes[0x2F] = D_KEY_PERIOD;
    keycodes[0x1E] = D_KEY_RIGHT_BRACKET;
    keycodes[0x29] = D_KEY_SEMICOLON;
    keycodes[0x2C] = D_KEY_SLASH;
    keycodes[0x0A] = D_KEY_WORLD_1;

    keycodes[0x33] = D_KEY_BACKSPACE;
    keycodes[0x39] = D_KEY_CAPS_LOCK;
    keycodes[0x75] = D_KEY_DELETE;
    keycodes[0x7D] = D_KEY_DOWN;
    keycodes[0x77] = D_KEY_END;
    keycodes[0x24] = D_KEY_ENTER;
    keycodes[0x35] = D_KEY_ESCAPE;
    keycodes[0x7A] = D_KEY_F1;
    keycodes[0x78] = D_KEY_F2;
    keycodes[0x63] = D_KEY_F3;
    keycodes[0x76] = D_KEY_F4;
    keycodes[0x60] = D_KEY_F5;
    keycodes[0x61] = D_KEY_F6;
    keycodes[0x62] = D_KEY_F7;
    keycodes[0x64] = D_KEY_F8;
    keycodes[0x65] = D_KEY_F9;
    keycodes[0x6D] = D_KEY_F10;
    keycodes[0x67] = D_KEY_F11;
    keycodes[0x6F] = D_KEY_F12;
    keycodes[0x69] = D_KEY_PRINT_SCREEN;
    keycodes[0x6B] = D_KEY_F14;
    keycodes[0x71] = D_KEY_F15;
    keycodes[0x6A] = D_KEY_F16;
    keycodes[0x40] = D_KEY_F17;
    keycodes[0x4F] = D_KEY_F18;
    keycodes[0x50] = D_KEY_F19;
    keycodes[0x5A] = D_KEY_F20;
    keycodes[0x73] = D_KEY_HOME;
    keycodes[0x72] = D_KEY_INSERT;
    keycodes[0x7B] = D_KEY_LEFT;
    keycodes[0x3A] = D_KEY_LEFT_ALT;
    keycodes[0x3B] = D_KEY_LEFT_CONTROL;
    keycodes[0x38] = D_KEY_LEFT_SHIFT;
    keycodes[0x37] = D_KEY_LEFT_SUPER;
    keycodes[0x6E] = D_KEY_MENU;
    keycodes[0x47] = D_KEY_NUM_LOCK;
    keycodes[0x79] = D_KEY_PAGE_DOWN;
    keycodes[0x74] = D_KEY_PAGE_UP;
    keycodes[0x7C] = D_KEY_RIGHT;
    keycodes[0x3D] = D_KEY_RIGHT_ALT;
    keycodes[0x3E] = D_KEY_RIGHT_CONTROL;
    keycodes[0x3C] = D_KEY_RIGHT_SHIFT;
    keycodes[0x36] = D_KEY_RIGHT_SUPER;
    keycodes[0x31] = D_KEY_SPACE;
    keycodes[0x30] = D_KEY_TAB;
    keycodes[0x7E] = D_KEY_UP;

    keycodes[0x52] = D_KEY_KP_0;
    keycodes[0x53] = D_KEY_KP_1;
    keycodes[0x54] = D_KEY_KP_2;
    keycodes[0x55] = D_KEY_KP_3;
    keycodes[0x56] = D_KEY_KP_4;
    keycodes[0x57] = D_KEY_KP_5;
    keycodes[0x58] = D_KEY_KP_6;
    keycodes[0x59] = D_KEY_KP_7;
    keycodes[0x5B] = D_KEY_KP_8;
    keycodes[0x5C] = D_KEY_KP_9;
    keycodes[0x45] = D_KEY_KP_ADD;
    keycodes[0x41] = D_KEY_KP_DECIMAL;
    keycodes[0x4B] = D_KEY_KP_DIVIDE;
    keycodes[0x4C] = D_KEY_KP_ENTER;
    keycodes[0x51] = D_KEY_KP_EQUAL;
    keycodes[0x43] = D_KEY_KP_MULTIPLY;
    keycodes[0x4E] = D_KEY_KP_SUBTRACT;
}

@end
