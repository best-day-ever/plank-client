#extension GL_OES_EGL_image_external : require
precision mediump float;

varying vec2 vTexCoord;

uniform samplerExternalOES uTexture;
uniform int uIdentityGbr8;

void main() {
    vec4 sample = texture2D(uTexture, vTexCoord);
    gl_FragColor = uIdentityGbr8 != 0 ? vec4(sample.b, sample.r, sample.g, 1.0) : sample;
}
