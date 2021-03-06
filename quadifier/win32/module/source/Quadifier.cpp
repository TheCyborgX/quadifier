#include <process.h>
#include <time.h>
#include <iomanip>
#include <sstream>
#include "Defines.h"
#include "Quadifier.h"
#include "Log.h"
#include "Settings.h"
#include "StereoUtil.h"
#include "DebugUtil.h"
#include "IDirect3DDevice9Proxy.h"
#include <GL/glext.h>
#include "WinMessage.h"

using namespace hive;
using namespace std;

//-----------------------------------------------------------------------------
//
// Copyright (C) 2012-14 James Ward, University of Hull
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
//    1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software.
//
//    2. If you use this software in a product, an acknowledgment in the
//    product documentation is required.
//
//    3. Altered source versions must be plainly marked as such, and must not
//    be misrepresented as being the original software.
//
//    4. This notice may not be removed or altered from any source
//    distribution.
//
//-----------------------------------------------------------------------------

/// user defined message indicating a new DX frame is available
static const UINT WM_USER_NEWFRAME = (WM_USER + 1);

//-----------------------------------------------------------------------------

Quadifier::Quadifier(
    IDirect3DDevice9 *device,
    IDirect3D9 *direct3D
) :
    m_device( device ),
    m_direct3D( direct3D )
{
    m_framesGL = 0;
    m_framesDX = 0;

    m_samplesDX = 0;
    m_samplesGL = 0;

    m_backBuffer = 0;
    m_drawBuffer = 0;
    m_readBuffer = 0;
    // m_target implicit
    m_stereoMode = false;
    m_firstFrameTimeGL = 0.0;
    m_lastFrameTimeGL = 0.0;
    m_quadListGL = 0;
    m_thread = 0;
    m_sourceWindow = 0;
    m_interopGLDX = 0;
    // m_newFrame implict
    // m_swapLock implict
    m_width  = 0;
    m_height = 0;
    m_initialised = false;

    // have we got stereo support?
    m_stereoAvailable = isOpenGLStereoAvailable();

    // set logging level
    // note: a few log messages will already have been output at this point
    Log::get().setLevel( Settings::get().logLevel );
}

//-----------------------------------------------------------------------------

Quadifier::~Quadifier()
{
    if (Log::info())
        Log::print( "~Quadifier\n" );

    // clear all the render targets
    for (unsigned i = 0; i < m_target.size(); ++i)
        m_target[i].clear();

    // release the backbuffer
    if ( m_backBuffer != 0 ) {
        m_backBuffer->Release();
        m_backBuffer = 0;
    }
}

//-----------------------------------------------------------------------------

void Quadifier::onCreateDX()
{
}//onCreateDX

//-----------------------------------------------------------------------------

void Quadifier::onPreClearDX(
    DWORD /*Count*/,
    CONST D3DRECT* /*pRects*/,
    DWORD /*Flags*/,
    D3DCOLOR /*Color*/,
    float /*Z*/,
    DWORD /*Stencil*/
) {
    if (Log::verbose()) Log::print( "onPreClearDX\n" );

    // is this the main display render target?
    if ( !isPresentedRenderTarget() ) return;

    if ( !m_initialised ) {
        // the first time we are called, we will create the resources
        // (render targets) - i.e. after the  parent window has been
        // created at the correct size
        createResources();
    }

    // start capturing DX drawing
    beginCapture();
}

//-----------------------------------------------------------------------------

void Quadifier::onPostClearDX()
{
}

//-----------------------------------------------------------------------------

void Quadifier::onPrePresentDX(
    CONST RECT * /*pSourceRect*/,
    CONST RECT * /*pDestRect*/,
    HWND /*hDestWindowOverride*/,
    CONST RGNDATA * /*pDirtyRegion*/
) {
    if (Log::verbose()) Log::print( "onPrePresentDX\n" );

    // get the current render target
    IDirect3DSurface9 *renderTarget = 0;
    if ( m_device->GetRenderTarget( 0, &renderTarget ) == S_OK ) {
        // release the render target (we are only storing its pointer as
        // a hash for comparison purposes)
        renderTarget->Release();

        // insert the render target in the set of presented targets
        // if not already present
        m_presentedTargets.insert(
            reinterpret_cast<unsigned>(renderTarget)
        );

        // verbose logging
        if (Log::verbose())
            Log::print() << "Presenting Render Target: " << renderTarget << endl;
    }

    // send frame to GL display thread
    // if we are in stereo mode, this will be the right eye
    // channel of a stereo pair, otherwise we are rendering 2D
    endCapture( m_stereoMode ? GL_BACK_RIGHT : GL_BACK );

    // signal that a new frame has been rendered
    if (Log::verbose()) Log::print( "sending new frame notification\n" );
    SendNotifyMessage( m_window.getHWND(), WM_USER_NEWFRAME, 0, 0 );
}//onPrePresentDX

//-----------------------------------------------------------------------------

void Quadifier::onPostPresentDX()
{
    if (Log::verbose()) Log::print( "onPostPresentDX\n" );

    // wait until the frame has been rendered out, to keep the OpenGL and
    // Direct3D threads synchronised (after a timeout we return anyway)
    m_frameDone.wait( 1000 );
}//onPostPresentDX

//-----------------------------------------------------------------------------

bool Quadifier::onPreSetViewportDX( CONST D3DVIEWPORT9 *pViewport )
{
    if (pViewport == nullptr) return true;

    // when we see SetViewport with a rectangle of (1,*,2,3) this is our
    // signal from the Quadifier script that right eye rendering has started
    if ( (pViewport->X      == 1) &&
         (pViewport->Width  == 2) &&
         (pViewport->Height == 3)
    ) {
        // call the handler to switch the stereo capture buffer
        onStereoSignal();
    }

    // return true to pass on the SetViewport call to Direct3D
    return true;
}//onPreSetViewportDX

//-----------------------------------------------------------------------------

bool Quadifier::onCreate()
{
    if (Log::info()) {
        // log some general information about the OpenGL renderer
        Log::print( "GL Version : " ) << glGetString( GL_VERSION ) << endl;
        Log::print( "GL Vendor  : " ) << glGetString( GL_VENDOR ) << endl;
        Log::print( "GL Renderer: " ) << glGetString( GL_RENDERER ) << endl;
    }

    // query number of antialiasing samples
    m_samplesGL = m_window.getSamples();
    if (Log::info())
        Log::print( "GL Samples : " ) << m_samplesGL << endl;

    // output pixel format index
    if (Log::info())
        Log::print() << "OpenGL pixel format = " << m_window.getPixelFormat() << endl;

    // query OpenGL texture size
    {
        GLint textureSize = 0;
        glGetIntegerv( GL_MAX_TEXTURE_SIZE, &textureSize );
        if (Log::info())
            Log::print() << "OpenGL maximum texture size = " << textureSize << endl;
    }

    bool success = false;

    // use textures or renderbuffers?
    bool useTexture = Settings::get().useTexture;

    if (!glx.load())
        Log::print( "error: failed to load GL extensions\n" );
    else do {
        if (Log::info()) Log::print( "loaded GL extensions\n" );

        if (Log::info()) Log::print( "creating GL/DX interop\n" );
        m_interopGLDX = glx.wglDXOpenDeviceNV( m_device );

        if ( m_interopGLDX == 0 ) {
            Log::print( "error: failed to create GL/DX interop\n" );
            break;
        }

        // select standard or multisampled GL texture mode
        GLenum textureMode = ( m_samplesGL > 1 ) ?
                GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;

        if (Log::info()) Log::print( "generating render buffers\n" );
        unsigned i=0;
        for (i=0; i<m_target.size(); ++i) {
            // are we using textures or renderbuffers?
            if ( useTexture ) {
                // using GL_TEXTURE_2D
                glGenTextures( 1, &m_target[i].texture );

                if ( m_target[i].texture == 0 ) {
                    Log::print( "error: failed to generate texture ID\n" );
                    break;
                }
            } else {
                // using GL_RENDERBUFFER
                glx.glGenRenderbuffers( 1, &m_target[i].renderBuffer );

                if ( m_target[i].renderBuffer == 0 ) {
                    Log::print( "error: failed to generate render buffer ID\n" );
                    break;
                }
            }

            // JDW - register ShareHandle for ATI/AMD interoperability
            if (Log::info())
                Log::print("Setting SharedHandle ") << m_target[i].shareHandle << endl;;
            if (glx.wglDXSetResourceShareHandleNV != 0) {
                glx.wglDXSetResourceShareHandleNV(m_target[i].surface, m_target[i].shareHandle);
            }
            else {
                Log::print("Failed to set SharedHandle: ") << m_target[i].shareHandle << endl;
                break;
            }

            if (Log::info())
                Log::print( "registering DX object " ) << i << endl;
            m_target[i].object = glx.wglDXRegisterObjectNV(
                m_interopGLDX,
                m_target[i].surface,
                useTexture ? m_target[i].texture : m_target[i].renderBuffer,
                useTexture ? textureMode : GL_RENDERBUFFER,
                WGL_ACCESS_READ_ONLY_NV
            );

            if ( m_target[i].object == 0 ) {
                DWORD error = GetLastError();
                Log::print( "error: wglDXRegisterObjectNV failed for render target: " )
                    << formatErrorMessage(error);
                break;
            }

            glx.glGenFramebuffers( 1, &m_target[i].frameBuffer );

            if ( m_target[i].frameBuffer == 0 ) {
                Log::print( "error: glGenFramebuffers failed\n" );
                break;
            }

            glx.glBindFramebuffer( GL_FRAMEBUFFER, m_target[i].frameBuffer );
            if (Log::info())
                Log::print() << "glBindFramebuffer = " << getGLErrorString() << endl;

            if ( useTexture ) {
                // using GL_TEXTURE_2D

                // important to lock before using glFramebufferTexture2D
                if ( glx.wglDXLockObjectsNV(m_interopGLDX, 1, &m_target[i].object) == GL_TRUE ) {

                    // attach colour buffer texture
                    glx.glFramebufferTexture2D(
                        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                        textureMode, m_target[i].texture, 0
                    );

                    // unlock
                    if (glx.wglDXUnlockObjectsNV(m_interopGLDX, 1, &m_target[i].object) != GL_TRUE ) {
                        Log::print() << "Error: UnLockObjectsNV for texture " << i << " failed " << endl;
                    }
                } else {
                    Log::print() << "Error: LockObjectsNV for texture " << i << " failed " << endl;
                }
            } else {
                // using GL_RENDERBUFFER

                // important to lock before using glFramebufferRenderbuffer
                if ( glx.wglDXLockObjectsNV(m_interopGLDX, 1, &m_target[i].object) == GL_TRUE ) {
                    // attach colour renderbuffer
                    glx.glFramebufferRenderbuffer(
                        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                        GL_RENDERBUFFER, m_target[i].renderBuffer
                    );

                    // unlock
                    if (glx.wglDXUnlockObjectsNV(m_interopGLDX, 1, &m_target[i].object) != GL_TRUE ) {
                        Log::print() << "Error: UnLockObjectsNV for renderBuffer " << i << " failed " << endl;
                    }
                } else {
                    Log::print() << "Error: LockObjectsNV for renderBuffer " << i << " failed " << endl;
                }

                glx.glBindRenderbuffer( GL_RENDERBUFFER, m_target[i].renderBuffer );

                // if we are logging informational messages
                if (Log::info()) {
                    // this table defines the renderbuffer parameters to be listed
                    struct {
                        GLenum name;
                        const char *text;
                    } table[] = {
                        { GL_RENDERBUFFER_WIDTH, "width" },
                        { GL_RENDERBUFFER_HEIGHT, "height" },
                        { GL_RENDERBUFFER_INTERNAL_FORMAT, "format" },
                        { GL_RENDERBUFFER_RED_SIZE, "red" },
                        { GL_RENDERBUFFER_GREEN_SIZE, "green" },
                        { GL_RENDERBUFFER_BLUE_SIZE, "blue" },
                        { GL_RENDERBUFFER_ALPHA_SIZE, "alpha" },
                        { GL_RENDERBUFFER_DEPTH_SIZE, "depth" },
                        { GL_RENDERBUFFER_STENCIL_SIZE, "stencil" },
                        { 0, 0 }
                    };

                    // query and log all the renderbuffer parameters
                    for (int p = 0; table[p].name != 0; ++p) {
                        GLint value = 0;
                        glx.glGetRenderbufferParameteriv( GL_RENDERBUFFER, table[p].name, &value );
                        Log::print( "renderBuffer." ) << table[p].text << " = " << value << endl;
                    }
                }

                glx.glBindRenderbuffer( GL_RENDERBUFFER, 0 );

            }

            // log the framebuffer status (should be GL_FRAMEBUFFER_COMPLETE)
            GLenum status = glx.glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if ((status != GL_FRAMEBUFFER_COMPLETE) || Log::info()) {
                Log::print() << "glCheckFramebufferStatus = " << GLFRAMEBUFFERSTATUStoString( status ) << endl;
                if (status == GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT) {
                    // JDW added for clarification:
                    Log::print() << "For ATI cards this may show GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT but gets corrected later.\n";
                }
            }
        }

        // successful only if all render buffers were created and initialised
        success = ( i == m_target.size() );
    } while (false_value);

    // default OpenGL settings
    glEnable( GL_COLOR_MATERIAL );
    glDisable( GL_LIGHTING );
    glDisable( GL_DEPTH_TEST );

    // default viewing system
    glMatrixMode( GL_MODELVIEW );
    glLoadIdentity();
    glMatrixMode( GL_PROJECTION );
    glLoadIdentity();

    return success;
}//onCreate

//-----------------------------------------------------------------------------

void Quadifier::onDestroy()
{
    // free OpenGL display list
    if ( m_quadListGL != 0 ) {
        glDeleteLists( m_quadListGL, 1 );
        m_quadListGL = 0;
    }

    if (Log::info()) {
        Log::print( "onDestroy\n" );

        Log::print( "DX presented targets = " ) << m_presentedTargets.size() << endl;

        Log::print( "GL frames = " ) << m_framesGL << endl;
        Log::print( "DX frames = " ) << m_framesDX << endl;

        // display a metric which indicates the ratio of DX to GL frames
        // (in stereo mode this should tend towards 200)
        if ( m_framesGL > 0 )
            Log::print("DX/GL metric = ")
                << 100 * m_framesDX / m_framesGL
                << endl;

        // display the final frame rate (number of GL frames per second)
        double elapsed = m_lastFrameTimeGL - m_firstFrameTimeGL;
        if ( elapsed > 0.01 )
            Log::print("Frame rate = ")
                << setprecision(2) << fixed
                << static_cast<double>(m_framesGL-1) / elapsed << " fps"
                << endl;
    }
}//onDestroy

//-----------------------------------------------------------------------------

void Quadifier::onPaint()
{
    // draw to default framebuffer
    glx.glBindFramebuffer( GL_DRAW_FRAMEBUFFER, 0 );

    // are we using textures?
    const bool useTexture = Settings::get().useTexture;

    // are we forced to use blit?
    const bool mustUseBlit = !Settings::get().matchOriginalMSAA;

    // save OpenGL state
    glPushAttrib( GL_ENABLE_BIT | GL_CURRENT_BIT );

    // enable texturing if required
    if ( useTexture ) {
        glEnable( GL_TEXTURE_2D );
        glColor3f( 1, 1, 1 );
    }

    // for each eye
    if (Log::verbose()) Log::print( "GL: rendering stereo frame\n" );
    for (int eye=0; eye<2; ++eye) {
        // get the GL draw buffer identifier for the last rendered frame
        // (i.e. the DX surface we are reading from)
        GLuint drawBuffer = m_target[m_readBuffer].drawBuffer;

        // select the GL draw buffer (GL_BACK or GL_BACK_LEFT or GL_BACK_RIGHT)
        if (Log::verbose()) {
            stringstream text;
            text << "GL: render " << m_readBuffer << " to "
                 << GLDRAWBUFFERtoString(drawBuffer) << endl;
            Log::print( text.str() );
        }
        glDrawBuffer( drawBuffer );

        // lock the shared DX/GL render target
        if ( (m_target[m_readBuffer].object != 0) && glx.wglDXLockObjectsNV(
            m_interopGLDX, 1,
            &m_target[m_readBuffer].object
        ) == GL_TRUE) {

            // are we rendering using textures or framebuffer blitting?
            if ( !useTexture || mustUseBlit ) {
                //-- render using framebuffer blitting        
                glx.glBindFramebuffer( GL_READ_FRAMEBUFFER, m_target[m_readBuffer].frameBuffer );
        
                // blit from the read framebuffer to the display framebuffer
                glx.glBlitFramebuffer(
                    0, 0, m_width, m_height,        // source rectangle
                    0, m_height, m_width, 0,        // destination: flip the image vertically
                    GL_COLOR_BUFFER_BIT,
                    GL_LINEAR
                );

            } else {
                //-- render using texture

                // bind the texture
                glBindTexture( GL_TEXTURE_2D, m_target[m_readBuffer].texture );

                // build our display list if it doesn't exist already
                if ( m_quadListGL == 0 ) {
                    // generate display list
                    m_quadListGL = glGenLists( 1 );

                    // draw a quad into the display list
                    glNewList( m_quadListGL, GL_COMPILE );
                        glBegin( GL_QUADS );
                            glTexCoord2i( 0, 0 );
                            glVertex3f( -1.0f, +1.0f, 0.0f );

                            glTexCoord2i( 1, 0 );
                            glVertex3f( +1.0f, +1.0f, 0.0f );

                            glTexCoord2i( 1, 1 );
                            glVertex3f( +1.0f, -1.0f, 0.0f );

                            glTexCoord2i( 0, 1 );
                            glVertex3f( -1.0f, -1.0f, 0.0f );
                        glEnd();
                    glEndList();
                }

                // draw a large textured quad
                if ( m_quadListGL != 0 )
                    glCallList( m_quadListGL );
            }

            // unlock the shared DX/GL target
            glx.wglDXUnlockObjectsNV(
                m_interopGLDX, 1,
                &m_target[m_readBuffer].object
            );
        } else
            Log::print( "unable to lock DX target on paint\n" );

        // pick next read buffer
        m_readBuffer = (m_readBuffer + 1) % m_target.size();

        // we are only rendering stereo if we have just rendered the left eye,
        // otherwise this must be a 2D frame and we can just exit the loop
        if ( drawBuffer != GL_BACK_LEFT ) break;
    }

    // restore OpenGL state
    glPopAttrib();

    // draw the left/right stereo channel indicator
    if ( Settings::get().stereoIndicator )
        drawStereoIndicator();

    // swap the buffers
    m_window.swapBuffers();

    // signal that we've processed one complete frame
    m_frameDone.signal();

    // in verbose mode, log the point at which GL swap occurs
    if (Log::verbose()) Log::print( "GLSWAP\n" );

    // performance statistics are collected in stereo mode
    if ( m_stereoMode ) {
        // record time-stamp of first/last frame
        if ( m_framesGL == 0 )
            m_firstFrameTimeGL = getTime();
        else
            m_lastFrameTimeGL = getTime();

        // count GL frames
        ++m_framesGL;
    }
}//onPaint

//-----------------------------------------------------------------------------

void Quadifier::onResize( UINT /*type*/, int w, int h )
{
    glViewport( 0, 0, w, h );
    m_width  = w;
    m_height = h;
    redraw();
}

//-----------------------------------------------------------------------------

void Quadifier::onStereoSignal()
{
    if (Log::verbose()) Log::print( "stereo signal\n" );

    // enable stereo mode and print a message to the log
    if ( !m_stereoMode ) {
        m_stereoMode = true;
        if ( Log::info() )
            Log::print( "Stereo enabled" ) << endl;
    }

    // end capturing and send the left stereo frame
    endCapture( GL_BACK_LEFT );

    // begin capturing the right stereo frame
    beginCapture();
}

void Quadifier::onIdle()
{
}

//-----------------------------------------------------------------------------

void Quadifier::redraw()
{
    // force an immediate paint
    m_window.invalidate();
    m_window.update();
}

//-----------------------------------------------------------------------------

void Quadifier::drawStereoIndicator()
{
    const int size = 32;

    GLint viewport[4] = {};
    glGetIntegerv( GL_VIEWPORT, viewport );

    // save OpenGL state
    glPushAttrib( GL_ENABLE_BIT | GL_CURRENT_BIT | GL_TRANSFORM_BIT );

        glMatrixMode( GL_PROJECTION );
        glPushMatrix();
        glLoadIdentity();
        glOrtho(
            0.0, static_cast<double>(viewport[2]),
            0.0, static_cast<double>(viewport[3]),
            -1.0, 1.0
        );

        glMatrixMode( GL_MODELVIEW );
        glPushMatrix();
        glLoadIdentity();

        glEnable( GL_COLOR_MATERIAL );

        glDrawBuffer( GL_BACK_LEFT );
        glColor3f( 0.f, 0.f, 1.f );
        glRecti( 0, 0, size, size );

        glDrawBuffer( GL_BACK_RIGHT );
        glColor3f( 1.f, 0.f, 0.f );
        glRecti( 0, 0, size, size );

        // restore modelview matrix
        glPopMatrix();

        // restore projection matrix
        glMatrixMode( GL_PROJECTION );
        glPopMatrix();

    // restore OpenGL state
    glPopAttrib();
}//drawStereoIndicator

//-----------------------------------------------------------------------------

LRESULT CALLBACK Quadifier::windowProc(
    HWND hWnd,      // handle to window
    UINT uMsg,      // message identifier
    WPARAM wParam,  // first message parameter
    LPARAM lParam   // second message parameter
) {
    switch (uMsg) {
    case WM_DESTROY:
        onDestroy();
        return 0L;

    case WM_USER_NEWFRAME:
        redraw();
        return 0L;

    case WM_PAINT:
        {
            PAINTSTRUCT paintStruct = {};
            (void)BeginPaint( hWnd, &paintStruct );
            onPaint();
            EndPaint( hWnd, &paintStruct );
        }
        return 0L;

    case WM_SIZE:
        onResize( static_cast<UINT>(wParam), LOWORD(lParam), HIWORD(lParam) );
        return 0L;

    // forward all these messages to the source window
    case WM_CLOSE:
    case WM_CHAR:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_MOUSEMOVE:
    case WM_MOUSEWHEEL:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
        PostMessage( m_sourceWindow, uMsg, wParam, lParam );
        return 0L;
    }

    return DefWindowProc( hWnd, uMsg, wParam, lParam );
}//windowProc

//-----------------------------------------------------------------------------

LRESULT CALLBACK WindowProc(
    HWND hWnd,      // handle to window
    UINT uMsg,      // message identifier
    WPARAM wParam,  // first message parameter
    LPARAM lParam   // second message parameter
) {
    if (uMsg == WM_NCCREATE) {
        // the WM_NCREATE is one of the first messages sent, and includes a
        // pointer to the LPCREATESTRUCT. in this structure, we put our window
        // class pointer at creation time. therefore we can retrieve it now,
        // and store in GWL_USERDATA so that we can retrieve it later.

        Quadifier *self = reinterpret_cast<Quadifier*>(
            reinterpret_cast<LPCREATESTRUCT>(lParam)->lpCreateParams
        );
        if ( self != 0 ) {
            SetWindowLongPtr(
                hWnd,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(self)
            );
        }
    }

    // attempt to get window pointer from GWL_USERDATA
    Quadifier *self = reinterpret_cast<Quadifier*>(
        GetWindowLongPtr( hWnd, GWLP_USERDATA )
    );
    
    // pass message onto the window, or fall-back to default
    if ( self != 0 )
        return self->windowProc( hWnd, uMsg, wParam, lParam );
    else
        return DefWindowProc( hWnd, uMsg, wParam, lParam );
}//WindowProc

//-----------------------------------------------------------------------------

LRESULT CALLBACK WindowProcSource(
    HWND hWnd,      // handle to window
    UINT uMsg,      // message identifier
    WPARAM wParam,  // first message parameter
    LPARAM lParam   // second message parameter
) {
    // get the old window proc pointer
    WNDPROC oldWindowProc = reinterpret_cast<WNDPROC>(
        GetWindowLongPtr( hWnd, GWLP_USERDATA )
    );

    //// log windows messages
    //Log::print()
    //    << "SRC " << windowsMessageToString( uMsg )
    //    << ',' << hex << wParam << ',' << lParam << dec << endl;

    switch (uMsg) {
    case WM_ERASEBKGND:
        // Ignore the WM_ERASEBKGND message
        return TRUE;
    }

    // call the original window proc
    return CallWindowProc( oldWindowProc, hWnd, uMsg, wParam, lParam );
}//WindowProcSource

//-----------------------------------------------------------------------------

unsigned __stdcall Quadifier::threadFunc( void *context )
{
    if (Log::info()) Log::print( "GL rendering thread started\n" );

    Quadifier *self = reinterpret_cast<Quadifier*>( context );

    if ( self == 0 ) {
        _endthreadex( 0 );
        return 0;
    }

    // Get the old window proc of the source window
    LONG_PTR oldWindowProc = GetWindowLongPtr( self->m_sourceWindow, GWLP_WNDPROC );

    // Store the oldWindowProc pointer in user data on the source window
    // (this pointer is later used to call the original window proc)
    SetWindowLongPtr( self->m_sourceWindow, GWLP_USERDATA, oldWindowProc );

    // Change the source window proc to point to our own WindowProcSource
    // function. This allows us to subclass the window in the source
    // application and intercept its messages.
    SetWindowLongPtr(
        self->m_sourceWindow, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(WindowProcSource)
    );

    // output GL window style
    DWORD dwStyle = WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_CHILD;

    // output GL window extended style
    DWORD dwExStyle = 0;

    // copy window rect from source window, so that the new window is
    // positioned directly on top of the original window
    RECT rect = {};
    GetClientRect( self->m_sourceWindow, &rect );

    // window creation attributes
    GLWindow::Attributes attributes;

    // if stereo is supported
    if ( self->m_stereoAvailable ) {
        // enable stereo
        attributes[WGL_STEREO_ARB] = GL_TRUE;
    }

    // set depth and stencil attributes
    attributes[WGL_DEPTH_BITS_ARB] = 0;
    attributes[WGL_STENCIL_BITS_ARB] = 0;

    // number of desired anti-alias samples to match DirectX
    unsigned desiredSamples = self->m_samplesDX;

    // do we want to match original number of multisamples used in DirectX?
    // if not, set it to zero
    if ( Settings::get().matchOriginalMSAA ) {
        if (Log::info())
            Log::print()
                << "matchOriginalMSAA is ENABLED\n"
                << "GL will attempt to use the same multisample format as DX\n";
    } else {
        if (Log::info())
            Log::print()
                << "matchOriginalMSAA is DISABLED\n"
                << "GL is not forced to use the sample multisample format as DX\n";

        desiredSamples = 0;
    }

    // optional: request the same number of multisamples as DirectX
    if ( desiredSamples > 0 ) {
        attributes[WGL_SAMPLE_BUFFERS_ARB] = GL_TRUE;
        attributes[WGL_SAMPLES_ARB] = desiredSamples;
    }

    // create our OpenGL window
    if ( !self->m_window.create(
        dwExStyle,
        L"Quadifier",
        dwStyle,
        0, 0,
        rect.right - rect.left,
        rect.bottom - rect.top,
        self->m_sourceWindow,
        0,
        WindowProc,
        self,
        attributes
    ) ) {
        Log::print( "error: failed to create OpenGL window\n" );
        _endthreadex( 0 );
        return 0;
    }

    // did we get the requested number of anti-alias samples?
    if ( self->m_window.getSamples() != desiredSamples ) {
        // warn the user in this case: this can result in failure when
        // the anti-aliasing in DirectX is higher than the GL context
        Log::print( "warning: unable to create OpenGL window with " )
            << desiredSamples << "x anti-alias samples\n"
            << "This may cause wglDXRegisterObjectNV to fail.\n"
            << "Please check if anti-aliasing is forced off in the driver settings.\n";
    }

    // call onCreate to carry out OpenGL setup
    if ( self->onCreate() ) {
        // show window without activating it
        self->m_window.show( SW_SHOWNA );

        MSG message;

        do {
            if ( PeekMessage( &message, NULL, 0, 0, PM_REMOVE ) ) {
                TranslateMessage( &message );
                DispatchMessage( &message );
            } else {
                // no message waiting: idle
                self->onIdle();
            }
        } while ( message.message != WM_QUIT );

        if (Log::info()) Log::print( "WM_QUIT\n" );
    } else {
        Log::print( "Failed when initialising OpenGL resources: exiting\n" );
        PostMessage( self->m_sourceWindow, WM_QUIT, 0, 0 );
    }

    _endthreadex( 0 );

    return 0;
}//ThreadFunc

//-----------------------------------------------------------------------------

double Quadifier::getTime() const {
    return static_cast<double>( clock() ) / static_cast<double>( CLOCKS_PER_SEC );
}

//-----------------------------------------------------------------------------

void Quadifier::beginCapture() {
    if (Log::verbose()) Log::print( "beginCapture\n" );

    // save the current viewport
    D3DVIEWPORT9 viewport = {};
    bool savedViewport = (m_device->GetViewport( &viewport ) == D3D_OK);

    // display render target parameters
    if (Log::verbose()) {
        stringstream text;
        text << "SetRenderTarget("
            << 0 << ','
            << m_target[m_drawBuffer].surface << ") "
            << "(drawBuffer==" << m_drawBuffer << ")\n";
        Log::print( text.str() );
    }

    // set the render target to the surface
    // any subsequent drawing (by the Direct3D application) will be rendered
    // into this surface
    // note: setting a new render target causes the viewport to be set to the
    // full size of the new render target
    if (m_device->SetRenderTarget( 0, m_target[m_drawBuffer].surface ) != D3D_OK) {
        Log::print( "Error Setting Render Target\n " );
        exit( 1 );
    }

    // restore the viewport
    if (savedViewport)
        m_device->SetViewport( &viewport );
}//beginCapture

//-----------------------------------------------------------------------------

void Quadifier::endCapture( GLuint drawBuffer ) {
    if (Log::verbose()) {
        std::stringstream text;
        text << "endCapture " << m_drawBuffer << " to " << GLDRAWBUFFERtoString( drawBuffer ) << endl;
        Log::print( text.str() );
    }

    // set the OpenGL draw buffer destination
    // the application has already rendered into this buffer, and here we are
    // just labelling the buffer with left/right/back as appropriate
    m_target[m_drawBuffer].drawBuffer = drawBuffer;

    // select next draw buffer
    m_drawBuffer = (m_drawBuffer + 1) % m_target.size();

    // count DX frames
    if ( m_stereoMode ) ++m_framesDX;
}//endCapture

//-----------------------------------------------------------------------------

bool Quadifier::isPresentedRenderTarget() const
{
    // ensure that we have a device
    if ( m_device == 0 ) return false;

    // receives a pointer to the render target (surface)
    IDirect3DSurface9 *renderTarget = 0;

    // receives hash generated from render target pointer
    unsigned hash = 0;

    // get the current render target
    if ( m_device->GetRenderTarget( 0, &renderTarget ) == S_OK ) {
        // if verbose logging is enabled
        if ( Log::verbose() ) {
            // render target description
            D3DSURFACE_DESC desc = {};

            // get the render target description
            if ( renderTarget->GetDesc( &desc ) == S_OK ) {
                // display render target details
                Log::print() << "Render target: " <<
                    renderTarget << ',' <<
                    D3DFORMATtoString( desc.Format ) << ',' <<
                    desc.Width << 'x' << desc.Height << endl;
            }
        }

        // use the renderTarget pointer as a simple hash
        hash = reinterpret_cast<unsigned>( renderTarget );

        // release render target
        renderTarget->Release();
    }

    // has this render target been presented?
    return ( m_presentedTargets.find(hash) != m_presentedTargets.end() );
}//isPresentedRenderTarget

//-----------------------------------------------------------------------------

void Quadifier::createResources()
{
    // in case the graphics driver settings are forcing multisampling (e.g. the
    // NVIDIA drivers are set to "override any application setting"), this
    // code attempts to query how many multisamples are in use
    int forcedSamples = 0;
    {
        // create a temporary OpenGL window with default pixel format
        GLWindow window;
        if ( window.create( 0, L"", 0, 0, 0, 8, 8, 0, 0, WindowProc, 0 ) ) {
            // query the number of samples from OpenGL
            forcedSamples = window.getSamples();
            window.destroy();
        }
        if (Log::info())
            Log::print( "OpenGL forced AA samples = " ) << forcedSamples << endl;
    }

    // convert number of samples to the Direct3D multisample type
    D3DMULTISAMPLE_TYPE forcedSamplesDX = D3DMULTISAMPLE_NONE;
    if ( forcedSamples > 1 ) {
        if ( forcedSamples > 16 ) forcedSamples = 16;
        forcedSamplesDX = static_cast<D3DMULTISAMPLE_TYPE>( forcedSamples );
    }

    if (Log::info())
        Log::print( "Create DX render targets\n" );

    if ( m_target[0].surface != 0 ) return;

    // store the window handle of the original source window (the window
    // in the parent Direct3D application)
    D3DDEVICE_CREATION_PARAMETERS parameters = {};
    m_device->GetCreationParameters( &parameters );
    m_sourceWindow = parameters.hFocusWindow;
    
    // get the adapter display mode
    D3DDISPLAYMODE displayMode = {};
    if ( m_direct3D->GetAdapterDisplayMode( D3DADAPTER_DEFAULT, &displayMode ) != S_OK ) {
        Log::print( "error: failed to get adapter display mode (using default)\n" );
        // fallback code
        displayMode.Format = D3DFMT_X8R8G8B8;
    }

    // assume no multisampling initially
    D3DSURFACE_DESC desc = {};
    desc.MultiSampleType = D3DMULTISAMPLE_NONE;

    // attempt to retrieve the multisample type from the render target
    IDirect3DSurface9 *renderTarget = 0;
    if ( m_device->GetRenderTarget( 0, &renderTarget ) == S_OK ) {
        // get the render target description
        if ( renderTarget->GetDesc( &desc ) == S_OK ) {
            // store viewport width and height
            m_width  = desc.Width;
            m_height = desc.Height;

            Log::print( "DX render target surface format: " ) <<
                D3DFORMATtoString( desc.Format ) << endl;
        } else {
            // failure: ensure the sample type is initialised to none
            desc.MultiSampleType = D3DMULTISAMPLE_NONE;
            Log::print( "error: failed to get render target description\n" );

            // fall back to GetViewport to get viewport dimensions
            D3DVIEWPORT9 viewport = {};
            m_device->GetViewport( &viewport );

            // store viewport width and height
            m_width  = viewport.Width;
            m_height = viewport.Height;
        }

        // release render target
        renderTarget->Release();
    } else {
        Log::print( "error: failed to get render target\n" );
    }

    if (Log::info()) {
        Log::print() << "DX viewport = "
            << m_width << 'x' << m_height << endl;
    }

    // multisampling level to use
    D3DMULTISAMPLE_TYPE multisampleType = desc.MultiSampleType;

    if (Log::info()) {
        // this is the original number of Direct3D samples used by the source
        Log::print( "DX multisample type = " ) << D3DMULTISAMPLE_TYPEtoString(
            multisampleType
            ) << endl;
    }

    // if the target has forced multisamples that are greater than the
    // Direct3D source, we force the source to use the same number to
    // ensure they have a compatible pixel format (otherwise the interop
    // seems to fail)
    if ( multisampleType < forcedSamplesDX ) {
        multisampleType = forcedSamplesDX;
        Log::print( "Forcing DX multisample type to: " ) <<
            D3DMULTISAMPLE_TYPEtoString( multisampleType ) << endl;
    }

    // convert multisampling level to an unsigned integer and store it
    // for later use when creating the target OpenGL window
    if ( 
        ( multisampleType >= D3DMULTISAMPLE_2_SAMPLES  ) &&
        ( multisampleType <= D3DMULTISAMPLE_16_SAMPLES )
    ) {
        m_samplesDX = static_cast<unsigned>( multisampleType );
    } else
        m_samplesDX = 0;

    LPDIRECT3DSURFACE9 depthStencilSurface = 0;
    D3DSURFACE_DESC depthStencilDesc = {};
    depthStencilDesc.Format = D3DFMT_D24S8;

    // display some information about the depth/stencil format
    // (purely informational, not required currently)
    if ( m_device->GetDepthStencilSurface( &depthStencilSurface ) == S_OK ) {
        // get the surface description
        if ( depthStencilSurface->GetDesc( &depthStencilDesc ) == S_OK ) {
            if (Log::info())
                Log::print( "DX depth/stencil surface format: " ) <<
                    D3DFORMATtoString( depthStencilDesc.Format ) << endl;
        } else {
            Log::print( "error: failed to get depth surface description\n" );
        }

        // release depth stencil surface
        depthStencilSurface->Release();
    } else
        Log::print( "error: failed to get depth stencil surface\n" );

    // create render target(s)
    for (unsigned i=0; i < m_target.size(); ++i) {
        // initialise share handle to NULL
        // JDW added for ATI compatibility
        m_target[i].shareHandle = NULL;

        // create render target
        if (m_device->CreateRenderTarget(
            m_width,
            m_height,
            displayMode.Format,
            multisampleType,
            0,
            FALSE,
            &m_target[i].surface,
            &m_target[i].shareHandle
        ) != S_OK) {
            Log::print("error: failed to create DX render target\n");
            break;
        }
    }

    // get the current render target and save for later use
    m_device->GetRenderTarget( 0, &m_backBuffer );

    // create window
    startRenderThread();

    // we have completed initialisation
    m_initialised = true;
}

//-----------------------------------------------------------------------------

void Quadifier::startRenderThread()
{
    if (Log::info())
        Log::print( "starting GL rendering thread\n" );

    if ( m_thread != 0 ) return;

    // create thread
    unsigned threadId = 0;

    m_thread = _beginthreadex(
        NULL,       // no security attributes (child cannot inherit handle)
        1024*1024,  // 1MB stack size
        threadFunc, // code to run on new thread
        this,       // pointer to host application class
        0,          // run immediately (could create suspended)
        &threadId   // OUT: returns thread ID
    );

    if (Log::info())
        Log::print() << "thread = " << hex << (unsigned)m_thread << dec << endl;
}

//-----------------------------------------------------------------------------
