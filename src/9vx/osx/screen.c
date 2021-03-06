#define Point OSXPoint
#define Rect OSXRect
#define Cursor OSXCursor
#include <Carbon/Carbon.h>
#include <QuickTime/QuickTime.h> // for full screen
#undef Rect
#undef Point
#undef Cursor
#undef offsetof
#undef nil

#include "u.h"
#include "lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "error.h"
#define Image IMAGE	/* kernel has its own Image */
#include <draw.h>
#include <memdraw.h>
#include <keyboard.h>
#include <cursor.h>
#include "screen.h"
#include "mouse.h"
#include "keycodes.h"
#include "nineball.h"

struct {
	Rectangle fullscreenr;
	Rectangle screenr;
	Memimage *screenimage;
	int isfullscreen;
	ulong fullscreentime;
	
	Point xy;
	int buttons;
	int kbuttons;

	CGDataProviderRef provider;
	MenuRef wmenu;
	MenuRef vmenu;
	WindowRef window;
	CGImageRef image;
	PasteboardRef snarf;
} osx;

enum
{
	WindowAttrs =
		kWindowCloseBoxAttribute |
		kWindowCollapseBoxAttribute |
		kWindowResizableAttribute |
		kWindowStandardHandlerAttribute |
		kWindowFullZoomAttribute
};

static void screenproc(void*);
static void eresized(int force);
static void fullscreen(void);
static void seticon(void);

static OSStatus quithandler(EventHandlerCallRef, EventRef, void*);
static OSStatus eventhandler(EventHandlerCallRef, EventRef, void*);
static OSStatus cmdhandler(EventHandlerCallRef, EventRef, void*);

enum
{
	CmdFullScreen = 1,
};

uchar*
attachscreen(Rectangle *r, ulong *chan, int *depth,
	int *width, int *softscreen, void **X)
{
	Memimage *m;

	if(osx.screenimage == nil){
		screeninit();
		if(osx.screenimage == nil)
			panic("cannot create OS X screen");
	}
	m = osx.screenimage;
	*r = m->r;
	*chan = m->chan;
	*depth = m->depth;
	*width = m->width;
	*X = nil;
	*softscreen = 1;
	return m->data->bdata;
}

void
_screeninit(void)
{
	CGRect cgr;
	OSXRect or;

	_memimageinit();

	ProcessSerialNumber psn = { 0, kCurrentProcess };
	TransformProcessType(&psn, kProcessTransformToForegroundApplication);
	SetFrontProcess(&psn);

	cgr = CGDisplayBounds(CGMainDisplayID());
	osx.fullscreenr = Rect(0, 0, cgr.size.width, cgr.size.height);
	
	InitCursor();
	
	// Create minimal menu with full-screen option.
	ClearMenuBar();
	CreateStandardWindowMenu(0, &osx.wmenu);
	InsertMenu(osx.wmenu, 0);
	MenuItemIndex ix;
	CreateNewMenu(1004, 0, &osx.vmenu);	// XXX 1004?
	SetMenuTitleWithCFString(osx.vmenu, CFSTR("View"));
	AppendMenuItemTextWithCFString(osx.vmenu,
		CFSTR("Full Screen"), 0, CmdFullScreen, &ix);
	SetMenuItemCommandKey(osx.vmenu, ix, 0, 'F');
	InsertMenu(osx.vmenu, GetMenuID(osx.wmenu));
	DrawMenuBar();

	// Create the window.
	or.left = 0;
	or.top = 50;
	or.bottom = Dy(osx.fullscreenr) - 200;
	or.right = Dx(osx.fullscreenr);
	CreateNewWindow(kDocumentWindowClass, WindowAttrs, &or, &osx.window);
	SetWindowTitleWithCFString(osx.window, CFSTR("Plan 9 VX"));
	seticon();

	// Set up the clip board.
	if(PasteboardCreate(kPasteboardClipboard, &osx.snarf) != noErr)
		panic("pasteboard create");

	// Explain in great detail which events we want to handle.
	// Why can't we just have one handler?
	const EventTypeSpec quits[] = {
		{ kEventClassApplication, kEventAppQuit }
	};
	const EventTypeSpec cmds[] = {
		{ kEventClassWindow, kEventWindowClosed },
		{ kEventClassWindow, kEventWindowBoundsChanged },
		{ kEventClassCommand, kEventCommandProcess }
	};
	const EventTypeSpec events[] = {
		{ kEventClassKeyboard, kEventRawKeyDown },
		{ kEventClassKeyboard, kEventRawKeyModifiersChanged },
		{ kEventClassKeyboard, kEventRawKeyRepeat },
		{ kEventClassMouse, kEventMouseDown },
		{ kEventClassMouse, kEventMouseUp },
		{ kEventClassMouse, kEventMouseMoved },
		{ kEventClassMouse, kEventMouseDragged },
		{ kEventClassMouse, kEventMouseWheelMoved },
	};

	InstallApplicationEventHandler(
		NewEventHandlerUPP(quithandler),
		nelem(quits), quits, nil, nil);

 	InstallApplicationEventHandler(
 		NewEventHandlerUPP(eventhandler),
		nelem(events), events, nil, nil);

	InstallWindowEventHandler(osx.window,
		NewEventHandlerUPP(cmdhandler),
		nelem(cmds), cmds, osx.window, nil);

	// Finally, put the window on the screen.
	ShowWindow(osx.window);
	ShowMenuBar();
	eresized(1);
	SelectWindow(osx.window);
	
	InitCursor();
}

static Psleep scr;

void
screeninit(void)
{
	plock(&scr);
	kproc("*screen*", screenproc, nil);
	while(osx.window == nil)
		psleep(&scr);
	punlock(&scr);
}

static void
screenproc(void *v)
{
	plock(&scr);
	_screeninit();
	pwakeup(&scr);
	punlock(&scr);
	RunApplicationEventLoop();
	iprint("screenproc exited!\n");
}

static OSStatus kbdevent(EventRef);
static OSStatus mouseevent(EventRef);

static OSStatus
cmdhandler(EventHandlerCallRef next, EventRef event, void *arg)
{
	return eventhandler(next, event, arg);
}

static OSStatus
quithandler(EventHandlerCallRef next, EventRef event, void *arg)
{
	restoretty();	// XXX: should we?
	exit(0);
	return 0;
}

static OSStatus
eventhandler(EventHandlerCallRef next, EventRef event, void *arg)
{
	OSStatus result;

	result = CallNextEventHandler(next, event);

	switch(GetEventClass(event)){
	case kEventClassKeyboard:
		return kbdevent(event);
	
	case kEventClassMouse:
		return mouseevent(event);
	
	case kEventClassCommand:;
		HICommand cmd;
		GetEventParameter(event, kEventParamDirectObject,
			typeHICommand, nil, sizeof cmd, nil, &cmd);
		switch(cmd.commandID){
		case kHICommandQuit:
			restoretty();	// XXX: should we?
			exit(0);
		
		case CmdFullScreen:
			fullscreen();
			break;
		
		default:
			return eventNotHandledErr;
		}
		break;
	
	case kEventClassWindow:;
		switch(GetEventKind(event)){
		case kEventWindowClosed:
			restoretty();	// XXX: should we?
			exit(0);
		
		case kEventWindowBoundsChanged:
			eresized(0);
			break;
		
		default:
			return eventNotHandledErr;
		}
		break;
	}
	
	return result;
}

static OSStatus
mouseevent(EventRef event)
{
	int wheel;
	OSXPoint op;
	
	GetEventParameter(event, kEventParamMouseLocation,
		typeQDPoint, 0, sizeof op, 0, &op);

	osx.xy = subpt(Pt(op.h, op.v), osx.screenr.min);
	wheel = 0;

	switch(GetEventKind(event)){
	case kEventMouseWheelMoved:;
		SInt32 delta;
		GetEventParameter(event, kEventParamMouseWheelDelta,
			typeSInt32, 0, sizeof delta, 0, &delta);
		if(delta > 0)
			wheel = 8;
		else
			wheel = 16;
		break;
	
	case kEventMouseDown:
	case kEventMouseUp:;
		UInt32 but, mod;
		GetEventParameter(event, kEventParamMouseChord,
			typeUInt32, 0, sizeof but, 0, &but);
		GetEventParameter(event, kEventParamKeyModifiers,
			typeUInt32, 0, sizeof mod, 0, &mod);
		
		// OS X swaps button 2 and 3
		but = (but & ~6) | ((but & 4)>>1) | ((but&2)<<1);
		
		// Apply keyboard modifiers and pretend it was a real mouse button.
		// (Modifiers typed while holding the button go into kbuttons,
		// but this one does not.)
		if(but == 1){
			if(mod & optionKey)
				but = 2;
			else if(mod & cmdKey)
				but = 4;
		}
		osx.buttons = but;
		break;

	case kEventMouseMoved:
	case kEventMouseDragged:
		break;
	
	default:
		return eventNotHandledErr;
	}

	mousetrack(osx.xy.x, osx.xy.y, osx.buttons|osx.kbuttons|wheel, msec());
	return noErr;	
}

static int keycvt[] =
{
	[QZ_IBOOK_ENTER] '\n',
	[QZ_RETURN] '\n',
	[QZ_ESCAPE] 27,
	[QZ_BACKSPACE] '\b',
	[QZ_LALT] Kalt,
	[QZ_LCTRL] Kctl,
	[QZ_LSHIFT] Kshift,
	[QZ_F1] KF+1,
	[QZ_F2] KF+2,
	[QZ_F3] KF+3,
	[QZ_F4] KF+4,
	[QZ_F5] KF+5,
	[QZ_F6] KF+6,
	[QZ_F7] KF+7,
	[QZ_F8] KF+8,
	[QZ_F9] KF+9,
	[QZ_F10] KF+10,
	[QZ_F11] KF+11,
	[QZ_F12] KF+12,
	[QZ_INSERT] Kins,
	[QZ_DELETE] 0x7F,
	[QZ_HOME] Khome,
	[QZ_END] Kend,
	[QZ_KP_PLUS] '+',
	[QZ_KP_MINUS] '-',
	[QZ_TAB] '\t',
	[QZ_PAGEUP] Kpgup,
	[QZ_PAGEDOWN] Kpgdown,
	[QZ_UP] Kup,
	[QZ_DOWN] Kdown,
	[QZ_LEFT] Kleft,
	[QZ_RIGHT] Kright,
	[QZ_KP_MULTIPLY] '*',
	[QZ_KP_DIVIDE] '/',
	[QZ_KP_ENTER] '\n',
	[QZ_KP_PERIOD] '.',
	[QZ_KP0] '0',
	[QZ_KP1] '1',
	[QZ_KP2] '2',
	[QZ_KP3] '3',
	[QZ_KP4] '4',
	[QZ_KP5] '5',
	[QZ_KP6] '6',
	[QZ_KP7] '7',
	[QZ_KP8] '8',
	[QZ_KP9] '9',
};

static void
kputc(int c)
{
	kbdputc(kbdq, c);
}

static OSStatus
kbdevent(EventRef event)
{
	char ch;
	UInt32 code;
	UInt32 mod;
	int k;

	GetEventParameter(event, kEventParamKeyMacCharCodes,
		typeChar, nil, sizeof ch, nil, &ch);
	GetEventParameter(event, kEventParamKeyCode,
		typeUInt32, nil, sizeof code, nil, &code);
	GetEventParameter(event, kEventParamKeyModifiers,
		typeUInt32, nil, sizeof mod, nil, &mod);

	switch(GetEventKind(event)){
	case kEventRawKeyDown:
	case kEventRawKeyRepeat:
		if(mod == cmdKey){
			if(ch == 'F' || ch == 'f'){
				if(osx.isfullscreen && msec() - osx.fullscreentime > 500)
					fullscreen();
				return noErr;
			}
			return eventNotHandledErr;
		}
		k = ch;
		if(code < nelem(keycvt) && keycvt[code])
			k = keycvt[code];
		if(k >= 0)
			latin1putc(k, kputc);
		else{
			UniChar uc;
			OSStatus s;
			
			s = GetEventParameter(event, kEventParamKeyUnicodes,
				typeUnicodeText, nil, sizeof uc, nil, &uc);
			if(s == noErr)
				kputc(uc);
		}
		break;

	case kEventRawKeyModifiersChanged:
		if(!osx.buttons && !osx.kbuttons){
			if(mod == optionKey)
				latin1putc(Kalt, kputc);
			break;
		}
		
		// If the mouse button is being held down, treat 
		// changes in the keyboard modifiers as changes
		// in the mouse buttons.
		osx.kbuttons = 0;
		if(mod & optionKey)
			osx.kbuttons |= 2;
		if(mod & cmdKey)
			osx.kbuttons |= 4;
		mousetrack(osx.xy.x, osx.xy.y, osx.buttons|osx.kbuttons, msec());
		break;
	}
	return noErr;
}

static void
eresized(int force)
{
	Memimage *m;
	OSXRect or;
	ulong chan;
	Rectangle r;
	int bpl;
	CGDataProviderRef provider;
	CGImageRef image;
	
	GetWindowBounds(osx.window, kWindowContentRgn, &or);
	r = Rect(or.left, or.top, or.right, or.bottom);
	if(Dx(r) == Dx(osx.screenr) && Dy(r) == Dy(osx.screenr) && !force){
		// No need to make new image.
		osx.screenr = r;
		return;
	}

	chan = XBGR32;
	m = allocmemimage(Rect(0, 0, Dx(r), Dy(r)), chan);
	if(m == nil)
		panic("allocmemimage: %r");
	if(m->data == nil)
		panic("m->data == nil");
	bpl = bytesperline(r, 32);
	provider = CGDataProviderCreateWithData(0,
		m->data->bdata, Dy(r)*bpl, 0);
	image = CGImageCreate(Dx(r), Dy(r), 8, 32, bpl,
		CGColorSpaceCreateDeviceRGB(),
		kCGImageAlphaNoneSkipLast,
		provider, 0, 0, kCGRenderingIntentDefault);
	CGDataProviderRelease(provider);	// CGImageCreate did incref

	mouserect = m->r;
	if(osx.image)
		CGImageRelease(osx.image);
	osx.image = image;
	osx.screenr = r;
	osx.screenimage = m;
	termreplacescreenimage(m);
	drawreplacescreenimage(m);	// frees old osx.screenimage if any
}

void
flushmemscreen(Rectangle r)
{
	CGRect cgr;
	CGContextRef context;
	CGImageRef subimg;

	QDBeginCGContext(GetWindowPort(osx.window), &context);
	
	cgr.origin.x = r.min.x;
	cgr.origin.y = r.min.y;
	cgr.size.width = Dx(r);
	cgr.size.height = Dy(r);
	subimg = CGImageCreateWithImageInRect(osx.image, cgr);
	cgr.origin.y = Dy(osx.screenr) - r.max.y; // XXX how does this make any sense?
	CGContextDrawImage(context, cgr, subimg);
	CGContextFlush(context);
	CGImageRelease(subimg);

	QDEndCGContext(GetWindowPort(osx.window), &context);
}

void
fullscreen(void)
{
	static Ptr restore;
	static WindowRef oldwindow;
	GDHandle device;

	if(osx.isfullscreen){
		EndFullScreen(restore, 0);
		osx.window = oldwindow;
		ShowWindow(osx.window);
		osx.isfullscreen = 0;
	}else{
		HideWindow(osx.window);
		oldwindow = osx.window;
		GetWindowGreatestAreaDevice(osx.window, kWindowTitleBarRgn, &device, nil);
		BeginFullScreen(&restore, device, 0, 0, &osx.window, 0, 0);
		osx.isfullscreen = 1;
		osx.fullscreentime = msec();
	}
	eresized(1);
}

void
setmouse(Point p)
{
	CGPoint cgp;
	
	cgp.x = p.x + osx.screenr.min.x;
	cgp.y = p.y + osx.screenr.min.y;
	CGWarpMouseCursorPosition(cgp);
}

void
setcursor(struct Cursor *c)
{
	OSXCursor oc;
	int i;

	// SetCursor is deprecated, but what replaces it?
	for(i=0; i<16; i++){
		oc.data[i] = ((ushort*)c->set)[i];
		oc.mask[i] = oc.data[i] | ((ushort*)c->clr)[i];
	}
	oc.hotSpot.h = - c->offset.x;
	oc.hotSpot.v = - c->offset.y;
	SetCursor(&oc);
}

void
getcolor(ulong i, ulong *r, ulong *g, ulong *b)
{
	ulong v;
	
	v = 0;
	*r = (v>>16)&0xFF;
	*g = (v>>8)&0xFF;
	*b = v&0xFF;
}

int
setcolor(ulong i, ulong r, ulong g, ulong b)
{
	/* no-op */
	return 0;
}


int
hwdraw(Memdrawparam *p)
{
	return 0;
}

struct {
	QLock lk;
	char buf[SnarfSize];
	Rune rbuf[SnarfSize];
	PasteboardRef apple;
} clip;

char*
getsnarf(void)
{
	char *s, *t;
	CFArrayRef flavors;
	CFDataRef data;
	CFIndex nflavor, ndata, j;
	CFStringRef type;
	ItemCount nitem;
	PasteboardItemID id;
	PasteboardSyncFlags flags;
	UInt32 i;

/*	fprint(2, "applegetsnarf\n"); */
	qlock(&clip.lk);
	clip.apple = osx.snarf;
	if(clip.apple == nil){
		if(PasteboardCreate(kPasteboardClipboard, &clip.apple) != noErr){
			fprint(2, "apple pasteboard create failed\n");
			qunlock(&clip.lk);
			return nil;
		}
	}
	flags = PasteboardSynchronize(clip.apple);
	if(flags&kPasteboardClientIsOwner){
		s = strdup(clip.buf);
		qunlock(&clip.lk);
		return s;
	}
	if(PasteboardGetItemCount(clip.apple, &nitem) != noErr){
		fprint(2, "apple pasteboard get item count failed\n");
		qunlock(&clip.lk);
		return nil;
	}
	for(i=1; i<=nitem; i++){
		if(PasteboardGetItemIdentifier(clip.apple, i, &id) != noErr)
			continue;
		if(PasteboardCopyItemFlavors(clip.apple, id, &flavors) != noErr)
			continue;
		nflavor = CFArrayGetCount(flavors);
		for(j=0; j<nflavor; j++){
			type = (CFStringRef)CFArrayGetValueAtIndex(flavors, j);
			if(!UTTypeConformsTo(type, CFSTR("public.utf16-plain-text")))
				continue;
			if(PasteboardCopyItemFlavorData(clip.apple, id, type, &data) != noErr)
				continue;
			ndata = CFDataGetLength(data);
			qunlock(&clip.lk);
			s = smprint("%.*S", ndata/2, (Rune*)CFDataGetBytePtr(data));
			CFRelease(flavors);
			CFRelease(data);
			for(t=s; *t; t++)
				if(*t == '\r')
					*t = '\n';
			return s;
		}
		CFRelease(flavors);
	}
	qunlock(&clip.lk);
	return nil;		
}

void
putsnarf(char *s)
{
	CFDataRef cfdata;
	PasteboardSyncFlags flags;

/*	fprint(2, "appleputsnarf\n"); */

	if(strlen(s) >= SnarfSize)
		return;
	qlock(&clip.lk);
	strcpy(clip.buf, s);
	runesnprint(clip.rbuf, nelem(clip.rbuf), "%s", s);
	clip.apple = osx.snarf;
	if(PasteboardClear(clip.apple) != noErr){
		fprint(2, "apple pasteboard clear failed\n");
		qunlock(&clip.lk);
		return;
	}
	flags = PasteboardSynchronize(clip.apple);
	if((flags&kPasteboardModified) || !(flags&kPasteboardClientIsOwner)){
		fprint(2, "apple pasteboard cannot assert ownership\n");
		qunlock(&clip.lk);
		return;
	}
	cfdata = CFDataCreate(kCFAllocatorDefault, 
		(uchar*)clip.rbuf, runestrlen(clip.rbuf)*2);
	if(cfdata == nil){
		fprint(2, "apple pasteboard cfdatacreate failed\n");
		qunlock(&clip.lk);
		return;
	}
	if(PasteboardPutItemFlavor(clip.apple, (PasteboardItemID)1,
		CFSTR("public.utf16-plain-text"), cfdata, 0) != noErr){
		fprint(2, "apple pasteboard putitem failed\n");
		CFRelease(cfdata);
		qunlock(&clip.lk);
		return;
	}
	/* CFRelease(cfdata); ??? */
	qunlock(&clip.lk);
}

static void
seticon(void)
{
	CGImageRef im;
	CGDataProviderRef d;

	d = CGDataProviderCreateWithData(nil, nineball_png, sizeof nineball_png, nil);
	im = CGImageCreateWithPNGDataProvider(d, nil, true, kCGRenderingIntentDefault);
	if(im)
		SetApplicationDockTileImage(im);
	CGImageRelease(im);
	CGDataProviderRelease(d);
}

