package org.lwjgl.opengl;

import java.nio.ByteBuffer;
import java.nio.IntBuffer;
import org.lwjgl.LWJGLException;
import org.lwjgl.LWJGLUtil;

final class WindowsContextImplementation implements ContextImplementation {
    static {
        System.err.println(
            "[OSITO-PD-JNI] replacement WindowsContextImplementation loaded");
        System.err.flush();
    }

    public ByteBuffer create(PeerInfo peerInfo, IntBuffer attribs,
                             ByteBuffer sharedContextHandle)
            throws LWJGLException {
        System.err.println("[OSITO-PD-JNI] create entry");
        System.err.flush();
        System.err.println("[OSITO-PD-JNI] peer lock begin");
        System.err.flush();
        ByteBuffer peerInfoHandle = peerInfo.lockAndGetHandle();
        System.err.println("[OSITO-PD-JNI] peer lock end");
        System.err.flush();
        try {
            System.err.println("[OSITO-PD-JNI] nCreate call");
            System.err.flush();
            ByteBuffer context = nCreate(peerInfoHandle, attribs,
                                         sharedContextHandle);
            System.err.println("[OSITO-PD-JNI] nCreate returned");
            System.err.flush();
            return context;
        } finally {
            peerInfo.unlock();
        }
    }

    private static native ByteBuffer nCreate(ByteBuffer peerInfoHandle,
                                              IntBuffer attribs,
                                              ByteBuffer sharedContextHandle)
            throws LWJGLException;

    native long getHGLRC(ByteBuffer contextHandle);

    native long getHDC(ByteBuffer contextHandle);

    public void swapBuffers() throws LWJGLException {
        ContextGL currentContext = ContextGL.getCurrentContext();
        if (currentContext == null)
            throw new IllegalStateException("No context is current");
        synchronized (currentContext) {
            PeerInfo peerInfo = currentContext.getPeerInfo();
            ByteBuffer peerInfoHandle = peerInfo.lockAndGetHandle();
            try {
                nSwapBuffers(peerInfoHandle);
            } finally {
                peerInfo.unlock();
            }
        }
    }

    private static native void nSwapBuffers(ByteBuffer peerInfoHandle)
            throws LWJGLException;

    public void releaseDrawable(ByteBuffer contextHandle)
            throws LWJGLException {
    }

    public void update(ByteBuffer contextHandle) {
    }

    public void releaseCurrentContext() throws LWJGLException {
        nReleaseCurrentContext();
    }

    private static native void nReleaseCurrentContext()
            throws LWJGLException;

    public void makeCurrent(PeerInfo peerInfo, ByteBuffer contextHandle)
            throws LWJGLException {
        ByteBuffer peerInfoHandle = peerInfo.lockAndGetHandle();
        try {
            nMakeCurrent(peerInfoHandle, contextHandle);
        } finally {
            peerInfo.unlock();
        }
    }

    private static native void nMakeCurrent(ByteBuffer peerInfoHandle,
                                            ByteBuffer contextHandle)
            throws LWJGLException;

    public boolean isCurrent(ByteBuffer contextHandle) throws LWJGLException {
        return nIsCurrent(contextHandle);
    }

    private static native boolean nIsCurrent(ByteBuffer contextHandle)
            throws LWJGLException;

    public void setSwapInterval(int value) {
        if (!nSetSwapInterval(value))
            LWJGLUtil.log("Failed to set swap interval");
        Util.checkGLError();
    }

    private static native boolean nSetSwapInterval(int value);

    public void destroy(PeerInfo peerInfo, ByteBuffer contextHandle)
            throws LWJGLException {
        nDestroy(contextHandle);
    }

    private static native void nDestroy(ByteBuffer contextHandle)
            throws LWJGLException;
}
