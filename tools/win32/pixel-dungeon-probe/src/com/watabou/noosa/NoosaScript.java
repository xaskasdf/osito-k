package com.watabou.noosa;

import com.badlogic.gdx.Gdx;
import com.watabou.glwrap.Attribute;
import com.watabou.glwrap.Quad;
import com.watabou.glwrap.Shader;
import com.watabou.glwrap.Uniform;
import com.watabou.glscripts.Script;
import com.watabou.pd.desktop.DebugDesktopLauncher;
import java.nio.FloatBuffer;
import java.nio.ShortBuffer;

public class NoosaScript extends Script {
    public Uniform uCamera;
    public Uniform uModel;
    public Uniform uTex;
    public Uniform uColorM;
    public Uniform uColorA;
    public Attribute aXY;
    public Attribute aUV;

    private Camera lastCamera;

    private static final String SHADER =
        "uniform mat4 uCamera;uniform mat4 uModel;" +
        "attribute vec4 aXYZW;attribute vec2 aUV;varying vec2 vUV;" +
        "void main() {  gl_Position = uCamera * uModel * aXYZW;" +
        "  vUV = aUV;}//\n" +
        "#ifdef GL_ES\nprecision mediump float;\n#endif\n" +
        "varying vec2 vUV;uniform sampler2D uTex;" +
        "uniform vec4 uColorM;uniform vec4 uColorA;" +
        "void main() {  gl_FragColor = texture2D( uTex, vUV )" +
        " * uColorM + uColorA;}";

    public NoosaScript() {
        DebugDesktopLauncher.log("NoosaScript Program constructor end");
        compile(shader());
        DebugDesktopLauncher.log("NoosaScript uniform begin uCamera");
        uCamera = uniform("uCamera");
        DebugDesktopLauncher.log("NoosaScript uniform end uCamera");
        uModel = uniform("uModel");
        uTex = uniform("uTex");
        uColorM = uniform("uColorM");
        uColorA = uniform("uColorA");
        DebugDesktopLauncher.log("NoosaScript attributes begin");
        aXY = attribute("aXYZW");
        aUV = attribute("aUV");
        DebugDesktopLauncher.log("NoosaScript constructor end");
    }

    @Override
    public void compile(String source) {
        String[] shaders = source.split("//\n");

        DebugDesktopLauncher.log("NoosaScript vertex createCompiled begin");
        Shader vertex = Shader.createCompiled(Shader.VERTEX, shaders[0]);
        DebugDesktopLauncher.log("NoosaScript vertex createCompiled end");
        attach(vertex);
        DebugDesktopLauncher.log("NoosaScript vertex attach end");

        DebugDesktopLauncher.log("NoosaScript fragment createCompiled begin");
        Shader fragment = Shader.createCompiled(Shader.FRAGMENT, shaders[1]);
        DebugDesktopLauncher.log("NoosaScript fragment createCompiled end");
        attach(fragment);
        DebugDesktopLauncher.log("NoosaScript fragment attach end");

        DebugDesktopLauncher.log("NoosaScript link begin");
        link();
        DebugDesktopLauncher.log("NoosaScript link end");
    }

    @Override
    public void use() {
        super.use();
        aXY.enable();
        aUV.enable();
    }

    public void drawElements(FloatBuffer vertices, ShortBuffer indices,
                             int size) {
        vertices.position(0);
        aXY.vertexPointer(2, 4, vertices);
        vertices.position(2);
        aUV.vertexPointer(2, 4, vertices);
        Gdx.gl.glDrawElements(4, size, 5123, indices);
    }

    public void drawQuad(FloatBuffer vertices) {
        vertices.position(0);
        aXY.vertexPointer(2, 4, vertices);
        vertices.position(2);
        aUV.vertexPointer(2, 4, vertices);
        Gdx.gl.glDrawElements(4, Quad.SIZE, 5123, Quad.INDICES_1);
    }

    public void drawQuadSet(FloatBuffer vertices, int size) {
        if (size == 0) {
            return;
        }
        vertices.position(0);
        aXY.vertexPointer(2, 4, vertices);
        vertices.position(2);
        aUV.vertexPointer(2, 4, vertices);
        Gdx.gl.glDrawElements(4, Quad.SIZE * size, 5123,
                              Quad.getIndices(size));
    }

    public void lighting(float rm, float gm, float bm, float am,
                         float ra, float ga, float ba, float aa) {
        uColorM.value4f(rm, gm, bm, am);
        uColorA.value4f(ra, ga, ba, aa);
    }

    public void resetCamera() {
        lastCamera = null;
    }

    public void camera(Camera camera) {
        if (camera == null) {
            camera = Camera.main;
        }
        if (camera != lastCamera) {
            lastCamera = camera;
            uCamera.valueM4(camera.matrix);
            Gdx.gl.glScissor(camera.x,
                             Game.height - camera.screenHeight - camera.y,
                             camera.screenWidth, camera.screenHeight);
        }
    }

    public static NoosaScript get() {
        return Script.use(NoosaScript.class);
    }

    protected String shader() {
        return SHADER;
    }
}
