#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vapoursynth/VSHelper.h>
#include <vapoursynth/VapourSynth.h>

static double radians(double degrees) { return degrees * M_PI / 180.0; }

typedef struct tweakData {
    VSNodeRef *node;
    const VSVideoInfo *vi;
    int adjustLuma, chromaHeight, chromaStride, chromaWidth, height, stride,
        width;
    uint8_t lut[256];
} tweakData;

typedef struct rgbData {
    VSNodeRef *node;
    const VSVideoInfo *vi;
    int height, stride, width;
    uint8_t rlut[256];
    uint8_t glut[256];
    uint8_t blut[256];
} rgbData;

static void VS_CC rgbFree(void *instanceData, VSCore *core,
                          const VSAPI *vsapi) {
    rgbData *d = (rgbData *)(instanceData);
    vsapi->freeNode(d->node);
    free(d);
}

static void VS_CC tweakFree(void *instanceData, VSCore *core,
                            const VSAPI *vsapi) {
    tweakData *d = (tweakData *)(instanceData);
    vsapi->freeNode(d->node);
    free(d);
}

static const VSFrameRef *VS_CC rgbGetFrame(int n, int activationReason,
                                           void **instanceData,
                                           void **frameData,
                                           VSFrameContext *frameCtx,
                                           VSCore *core, const VSAPI *vsapi) {
    rgbData *d = (rgbData *)*instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src = vsapi->getFrameFilter(n, d->node, frameCtx);
        VSFrameRef *dst =
            vsapi->newVideoFrame(d->vi->format, d->width, d->height, src, core);

        const uint8_t *rsrc = vsapi->getReadPtr(src, 0);
        uint8_t *rdst = vsapi->getWritePtr(dst, 0);
        for (int y = 0; y < d->height; y++) {
            for (int x = 0; x < d->width; x++) rdst[x] = d->rlut[rsrc[x]];
            rsrc += d->stride;
            rdst += d->stride;
        }

        const uint8_t *gsrc = vsapi->getReadPtr(src, 1);
        uint8_t *gdst = vsapi->getWritePtr(dst, 1);
        for (int y = 0; y < d->height; y++) {
            for (int x = 0; x < d->width; x++) gdst[x] = d->glut[gsrc[x]];
            gsrc += d->stride;
            gdst += d->stride;
        }

        const uint8_t *bsrc = vsapi->getReadPtr(src, 2);
        uint8_t *bdst = vsapi->getWritePtr(dst, 2);
        for (int y = 0; y < d->height; y++) {
            for (int x = 0; x < d->width; x++) bdst[x] = d->blut[bsrc[x]];
            bsrc += d->stride;
            bdst += d->stride;
        }

        vsapi->freeFrame(src);

        return dst;
    }

    return NULL;
}

static const VSFrameRef *VS_CC tweakGetFrame(int n, int activationReason,
                                             void **instanceData,
                                             void **frameData,
                                             VSFrameContext *frameCtx,
                                             VSCore *core, const VSAPI *vsapi) {
    tweakData *d = (tweakData *)*instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src = vsapi->getFrameFilter(n, d->node, frameCtx);
        VSFrameRef *dst =
            vsapi->newVideoFrame(d->vi->format, d->width, d->height, src, core);

        const uint8_t *ysrc = vsapi->getReadPtr(src, 0);
        uint8_t *ydst = vsapi->getWritePtr(dst, 0);

        if (d->adjustLuma) {
            for (int y = 0; y < d->height; y++) {
                for (int x = 0; x < d->width; x++) ydst[x] = d->lut[ysrc[x]];
                ysrc += d->stride;
                ydst += d->stride;
            }
        } else
            vs_bitblt(ydst, d->stride, ysrc, d->stride, d->width, d->height);

        vs_bitblt(vsapi->getWritePtr(dst, 1), d->chromaStride,
                  vsapi->getReadPtr(src, 1), d->chromaStride, d->chromaWidth,
                  d->chromaHeight);
        vs_bitblt(vsapi->getWritePtr(dst, 2), d->chromaStride,
                  vsapi->getReadPtr(src, 2), d->chromaStride, d->chromaWidth,
                  d->chromaHeight);

        vsapi->freeFrame(src);

        return dst;
    }

    return NULL;
}

static void VS_CC rgbInit(VSMap *in, VSMap *out, void **instanceData,
                          VSNode *node, VSCore *core, const VSAPI *vsapi) {
    rgbData *d = (rgbData *)*instanceData;
    vsapi->setVideoInfo(d->vi, 1, node);
}

static void VS_CC tweakInit(VSMap *in, VSMap *out, void **instanceData,
                            VSNode *node, VSCore *core, const VSAPI *vsapi) {
    tweakData *d = (tweakData *)*instanceData;
    vsapi->setVideoInfo(d->vi, 1, node);
}

static VSNodeRef *tweakChroma(VSNodeRef *node, double mult1, double mult2,
                              VSColorFamily colorFamily, VSPlugin *std,
                              VSMap *out, const VSAPI *vsapi) {
    char buf[128];

    if (std == NULL) {
        vsapi->setError(out, "Tweak: standard plugin not found");
        return NULL;
    }
    VSMap *args = vsapi->createMap();
    vsapi->propSetNode(args, "clips", node, paReplace);
    vsapi->propSetInt(args, "planes", 1, paReplace);
    vsapi->propSetInt(args, "colorfamily", cmGray, paReplace);

    VSMap *uPlaneMap = vsapi->invoke(std, "ShufflePlanes", args);
    if (vsapi->getError(uPlaneMap)) {
        vsapi->setError(out, vsapi->getError(uPlaneMap));
        vsapi->freeMap(uPlaneMap);
        vsapi->freeMap(args);
        return NULL;
    }
    VSNodeRef *usrc = vsapi->propGetNode(uPlaneMap, "clip", 0, NULL);
    vsapi->freeMap(uPlaneMap);

    vsapi->propSetInt(args, "planes", 2, paReplace);
    VSMap *vPlaneMap = vsapi->invoke(std, "ShufflePlanes", args);
    if (vsapi->getError(vPlaneMap)) {
        vsapi->setError(out, vsapi->getError(vPlaneMap));
        vsapi->freeMap(vPlaneMap);
        vsapi->freeMap(args);
        return NULL;
    }
    VSNodeRef *vsrc = vsapi->propGetNode(vPlaneMap, "clip", 0, NULL);
    vsapi->freeMap(vPlaneMap);

    vsapi->clearMap(args);
    vsapi->propSetNode(args, "clips", usrc, paReplace);
    vsapi->propSetNode(args, "clips", vsrc, paAppend);

    snprintf(buf, 128, "x 128 - %f * y 128 - %f * + 128 + 0 max 255 min", mult1,
             mult2);
    vsapi->propSetData(args, "expr", buf, -1, paReplace);

    uPlaneMap = vsapi->invoke(std, "Expr", args);
    if (vsapi->getError(uPlaneMap)) {
        vsapi->setError(out, vsapi->getError(uPlaneMap));
        vsapi->freeMap(uPlaneMap);
        vsapi->freeMap(args);
        vsapi->freeNode(node);
        vsapi->freeNode(usrc);
        vsapi->freeNode(vsrc);
        return NULL;
    }
    VSNodeRef *udst = vsapi->propGetNode(uPlaneMap, "clip", 0, NULL);
    vsapi->freeMap(uPlaneMap);

    snprintf(buf, 128, "y 128 - %f * x 128 - %f * - 128 + 0 max 255 min", mult1,
             mult2);
    vsapi->propSetData(args, "expr", buf, -1, paReplace);

    vPlaneMap = vsapi->invoke(std, "Expr", args);
    vsapi->freeNode(usrc);
    vsapi->freeNode(vsrc);
    if (vsapi->getError(vPlaneMap)) {
        vsapi->setError(out, vsapi->getError(vPlaneMap));
        vsapi->freeMap(vPlaneMap);
        vsapi->freeMap(args);
        vsapi->freeNode(node);
        return NULL;
    }
    VSNodeRef *vdst = vsapi->propGetNode(vPlaneMap, "clip", 0, NULL);
    vsapi->freeMap(vPlaneMap);

    vsapi->clearMap(args);
    vsapi->propSetNode(args, "clips", node, paReplace);
    vsapi->propSetNode(args, "clips", udst, paAppend);
    vsapi->propSetNode(args, "clips", vdst, paAppend);
    vsapi->propSetInt(args, "planes", 0, paReplace);
    vsapi->propSetInt(args, "planes", 0, paAppend);
    vsapi->propSetInt(args, "planes", 0, paAppend);
    vsapi->propSetInt(args, "colorfamily", colorFamily, paReplace);

    VSMap *tweakedMap = vsapi->invoke(std, "ShufflePlanes", args);
    vsapi->freeMap(args);
    vsapi->freeNode(node);
    vsapi->freeNode(udst);
    vsapi->freeNode(vdst);
    if (vsapi->getError(tweakedMap)) {
        vsapi->setError(out, vsapi->getError(tweakedMap));
        vsapi->freeMap(tweakedMap);
        return NULL;
    }
    VSNodeRef *tweaked = vsapi->propGetNode(tweakedMap, "clip", 0, NULL);
    vsapi->freeMap(tweakedMap);

    return tweaked;
}

static void VS_CC tweakCreate(const VSMap *in, VSMap *out, void *userData,
                              VSCore *core, const VSAPI *vsapi) {
    tweakData d;
    tweakData *data;
    int err;

    VSNodeRef *node = vsapi->propGetNode(in, "clip", 0, NULL);
    d.vi = vsapi->getVideoInfo(node);

    const VSFrameRef *frame = vsapi->getFrame(0, node, NULL, 0);
    d.stride = vsapi->getStride(frame, 0);
    d.chromaStride = vsapi->getStride(frame, 1);
    vsapi->freeFrame(frame);

    d.height = d.vi->height;
    d.width = d.vi->width;
    d.chromaHeight = d.height >> d.vi->format->subSamplingH;
    d.chromaWidth = d.width >> d.vi->format->subSamplingW;

    double hue = vsapi->propGetFloat(in, "hue", 0, &err);
    if (err) hue = 0.0;
    double sat = vsapi->propGetFloat(in, "sat", 0, &err);
    if (err) sat = 1.0;

    int numLuma, incr;

    const double *lumaAdjust = vsapi->propGetFloatArray(in, "luma", &err);
    if (err) {
        d.adjustLuma = 0;
        numLuma = 0;
    } else {
        d.adjustLuma = 1;
        numLuma = vsapi->propNumElements(in, "luma");
        if (numLuma != 1) incr = 256 / (numLuma - 1);
    }

    if (hue != 0.0 || sat != 1.0) {
        hue = radians(hue);
        d.node = tweakChroma(
            node, cos(hue) * sat, sin(hue) * sat, d.vi->format->colorFamily,
            vsapi->getPluginById("com.vapoursynth.std", core), out, vsapi);

        if (d.node == NULL) return;
    } else
        d.node = node;

    double min_mult, max_mult, a, b, val;
    int start, end;

    if (numLuma == 1) {
        double mult = 1.0 + (lumaAdjust[0] / 100.0);
        for (int i = 0; i < 256; i++) {
            val = (i * mult) + 0.5;
            d.lut[i] = (val < 255.0) ? (uint8_t)val : 255;
        }
    } else {
        for (int i = 0; i < numLuma; i++) {
            start = i * incr;
            if (i == numLuma - 1)
                end = 255;
            else
                end = (i + 1) * incr - 1;
            min_mult = 1.0 + (lumaAdjust[i] / 100.0);
            max_mult = 1.0 + (lumaAdjust[i + 1] / 100.0);
            a = (max_mult - min_mult) / (end - start);
            b = max_mult - a * end;

            for (int j = start; j <= end; j++) {
                val = ((a * j + b) * j) + 0.5;
                d.lut[j] = (val < 255.0) ? (uint8_t)val : 255;
            }
        }
    }

    data = malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "Tweak", tweakInit, tweakGetFrame, tweakFree,
                        fmParallel, 0, data, core);
}

static void VS_CC rgbCreate(const VSMap *in, VSMap *out, void *userData,
                            VSCore *core, const VSAPI *vsapi) {
    rgbData d;
    rgbData *data;
    int err;

    VSNodeRef *node = vsapi->propGetNode(in, "clip", 0, NULL);

    if (vsapi->getVideoInfo(node)->format->colorFamily != cmRGB) {
        VSMap *args = vsapi->createMap();
        VSPlugin *resize = vsapi->getPluginById("com.vapoursynth.resize", core);
        vsapi->propSetNode(args, "clip", node, paReplace);
        vsapi->freeNode(node);
        vsapi->propSetInt(args, "format", pfRGB24, paReplace);
        vsapi->propSetInt(args, "matrix_in", 1, paReplace);
        VSMap *image = vsapi->invoke(resize, "Spline36", args);
        vsapi->freeMap(args);
        if (vsapi->getError(image)) {
            vsapi->setError(out, vsapi->getError(image));
            vsapi->freeMap(image);
            return;
        }
        d.node = vsapi->propGetNode(image, "clip", 0, NULL);
        vsapi->freeMap(image);
    } else
        d.node = node;

    d.vi = vsapi->getVideoInfo(d.node);

    const VSFrameRef *frame = vsapi->getFrame(0, d.node, NULL, 0);
    d.stride = vsapi->getStride(frame, 0);
    vsapi->freeFrame(frame);

    d.height = d.vi->height;
    d.width = d.vi->width;

    double red = vsapi->propGetFloat(in, "red", 0, &err);
    if (err) red = 1.0;
    double green = vsapi->propGetFloat(in, "green", 0, &err);
    if (err) green = 1.0;
    double blue = vsapi->propGetFloat(in, "blue", 0, &err);
    if (err) blue = 1.0;

    double val;
    for (int i = 0; i < 256; i++) {
        val = (i * red) + 0.5;
        d.rlut[i] = (val < 255.0) ? (uint8_t)val : 255;
        val = (i * green) + 0.5;
        d.glut[i] = (val < 255.0) ? (uint8_t)val : 255;
        val = (i * blue) + 0.5;
        d.blut[i] = (val < 255.0) ? (uint8_t)val : 255;
    }

    data = malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "RGB", rgbInit, rgbGetFrame, rgbFree,
                        fmParallel, 0, data, core);
}

VS_EXTERNAL_API(void)
VapourSynthPluginInit(VSConfigPlugin configFunc,
                      VSRegisterFunction registerFunc, VSPlugin *plugin) {
    configFunc("xyz.noctem.tweak", "tweak",
               "Filter for luma and chroma adjustment", VAPOURSYNTH_API_VERSION,
               1, plugin);
    registerFunc("Tweak",
                 "clip:clip;sat:float:opt;hue:float:opt;luma:float[]:opt;",
                 tweakCreate, NULL, plugin);
    registerFunc("RGB",
                 "clip:clip;red:float:opt;green:float:opt;blue:float:opt;",
                 rgbCreate, NULL, plugin);
}
