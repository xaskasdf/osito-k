package com.watabou.glwrap;

import com.badlogic.gdx.Gdx;
import com.badlogic.gdx.utils.BufferUtils;
import com.watabou.pd.desktop.DebugDesktopLauncher;
import java.nio.IntBuffer;

public class Shader {
    public static final int VERTEX = 35633;
    public static final int FRAGMENT = 35632;

    private int handle;

    public Shader(int type) {
        DebugDesktopLauncher.log("Shader glCreateShader begin type=" + type);
        handle = Gdx.gl.glCreateShader(type);
        DebugDesktopLauncher.log("Shader glCreateShader end handle=" + handle);
    }

    public int handle() {
        return handle;
    }

    public void source(String source) {
        DebugDesktopLauncher.log("Shader glShaderSource begin handle=" + handle
                                 + " length=" + source.length());
        Gdx.gl.glShaderSource(handle, source);
        DebugDesktopLauncher.log("Shader glShaderSource end handle=" + handle);
    }

    public void compile() {
        DebugDesktopLauncher.log("Shader glCompileShader begin handle=" + handle);
        Gdx.gl.glCompileShader(handle);
        DebugDesktopLauncher.log("Shader glCompileShader end handle=" + handle);

        DebugDesktopLauncher.log("Shader status buffer begin handle=" + handle);
        IntBuffer status = BufferUtils.newIntBuffer(1);
        DebugDesktopLauncher.log("Shader status buffer end handle=" + handle);

        DebugDesktopLauncher.log("Shader glGetShaderiv begin handle=" + handle);
        Gdx.gl.glGetShaderiv(handle, 35713, status);
        DebugDesktopLauncher.log("Shader glGetShaderiv end handle=" + handle);

        int compiled = status.get();
        DebugDesktopLauncher.log("Shader compile status handle=" + handle
                                 + " value=" + compiled);
        if (compiled == 0) {
            DebugDesktopLauncher.log("Shader glGetShaderInfoLog begin handle="
                                     + handle);
            String info = Gdx.gl.glGetShaderInfoLog(handle);
            DebugDesktopLauncher.log("Shader glGetShaderInfoLog end handle="
                                     + handle + " log=" + info);
            throw new Error(info);
        }
    }

    public void delete() {
        Gdx.gl.glDeleteShader(handle);
    }

    public static Shader createCompiled(int type, String source) {
        Shader shader = new Shader(type);
        shader.source(source);
        shader.compile();
        return shader;
    }
}
