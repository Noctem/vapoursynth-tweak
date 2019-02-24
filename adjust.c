#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include <vapoursynth/VapourSynth.h>
#include <vapoursynth/VSHelper.h>

static double radians(double degrees) { return degrees * M_PI / 180.0; }

typedef struct adjustData {
    VSNodeRef *node;
    const VSVideoInfo *vi;
    int adjustLuma, chromaHeight, chromaStride, chromaWidth, height, stride, width;
    double mult1, mult2;
    uint8_t lut[256];
} adjustData;

static void VS_CC adjustFree(void *instanceData, VSCore *core,
                             const VSAPI *vsapi) {
    adjustData *d = (adjustData *)(instanceData);
    vsapi->freeNode(d->node);
    free(d);
}

static const VSFrameRef *VS_CC adjustGetFrame(
    int n, int activationReason, void **instanceData, void **frameData,
    VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    adjustData *d = (adjustData *)*instanceData;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src = vsapi->getFrameFilter(n, d->node, frameCtx);
        VSFrameRef *dst = vsapi->newVideoFrame(d->vi->format, d->width, d->height, src, core);

        const uint8_t *ysrc = vsapi->getReadPtr(src, 0);
        uint8_t *ydst = vsapi->getWritePtr(dst, 0);

        if (d->adjustLuma) {
            for (int y = 0; y < d->height; y++) {
                for (int x = 0; x < d->width; x++)
                    ydst[x] = d->lut[ysrc[x]];
                ysrc += d->stride;
                ydst += d->stride;
            }
        } else
            vs_bitblt(ydst, d->stride, ysrc, d->stride, d->width, d->height);

        const uint8_t *usrc = vsapi->getReadPtr(src, 1);
        const uint8_t *vsrc = vsapi->getReadPtr(src, 2);
        uint8_t *udst = vsapi->getWritePtr(dst, 1);
        uint8_t *vdst = vsapi->getWritePtr(dst, 2);

        if (d->mult1 != 1.0 || d->mult2 != 0.0) {
            double u, u2, v, v2;

            for (int y = 0; y < d->chromaHeight; y++) {
                for (int x = 0; x < d->chromaWidth; x++) {
                    u = usrc[x] - 128.0;
                    v = vsrc[x] - 128.0;
                    u2 = u * d->mult1 + v * d->mult2 + 128.5;
                    v2 = v * d->mult1 - u * d->mult2 + 128.5;
                    udst[x] = (u2 < 255.0) ? ((u2 >= 1.0) ? (uint8_t)u2 : 0) : 255;
                    vdst[x] = (v2 < 255.0) ? ((v2 >= 1.0) ? (uint8_t)v2 : 0) : 255;
                }
                usrc += d->stride;
                vsrc += d->stride;
                udst += d->stride;
                vdst += d->stride;
            }
        } else {
            vs_bitblt(udst, d->chromaStride, usrc, d->chromaStride, d->chromaWidth, d->chromaHeight);
            vs_bitblt(vdst, d->chromaStride, vsrc, d->chromaStride, d->chromaWidth, d->chromaHeight);
        }
        vsapi->freeFrame(src);

        return dst;
    }

    return NULL;
}

static void VS_CC adjustInit(VSMap *in, VSMap *out, void **instanceData,
                            VSNode *node, VSCore *core, const VSAPI *vsapi) {
    adjustData *d = (adjustData *)*instanceData;
    vsapi->setVideoInfo(d->vi, 1, node);
}

static void VS_CC adjustCreate(const VSMap *in, VSMap *out, void *userData,
                               VSCore *core, const VSAPI *vsapi) {
    adjustData d;
    adjustData *data;
    int err;

    d.node = vsapi->propGetNode(in, "clip", 0, NULL);
    d.vi = vsapi->getVideoInfo(d.node);

    const VSFrameRef *frame = vsapi->getFrame(0, d.node, NULL, 0);
    d.stride = vsapi->getStride(frame, 0);
    d.chromaStride = vsapi->getStride(frame, 1);
    vsapi->freeFrame(frame);

    d.height = d.vi->height;
    d.width = d.vi->width;

    if (d.vi->format->subSamplingH || d.vi->format->subSamplingW) {
        d.chromaHeight = d.height >> d.vi->format->subSamplingH;
        d.chromaWidth = d.width >> d.vi->format->subSamplingW;
    } else {
        d.chromaHeight = d.height;
        d.chromaWidth = d.width;
    }

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
        if (numLuma != 1)
            incr = 256 / (numLuma - 1);
    }

    hue = radians(hue);
    d.mult1 = cos(hue) * sat;
    d.mult2 = sin(hue) * sat;

    double min_mult, max_mult, a, b, val;
    int start, end;
    
    if (numLuma == 1) {
        double mult = 1.0 + (lumaAdjust[0] / 100.0);
        for (int i = 0; i < 256; i++) {
            val = (i * mult) + 0.5;
            d.lut[i] = (val < 255.0) ? ((val >= 1.0) ? (uint8_t)val : 0) : 255;
        }
    } else {
        for (int i = 0; i < numLuma - 1; i++) {
            start = i * incr;
            if (i == numLuma - 1)
                end = 255;
            else
                end = (i + 1) * incr - 1;
            min_mult = 1.0 + (lumaAdjust[i] / 100.0);
            max_mult = 1.0 + (lumaAdjust[i+1] / 100.0);
            a = (max_mult - min_mult) / (end - start);
            b = max_mult - a * end;

            for (int j = start; j <= end; j++) {
                val = ((a * j + b) * j) + 0.5;
                d.lut[j] = (val < 255.0) ? ((val >= 1.0) ? (uint8_t)val : 0) : 255;
            }
        }
    }

    data = malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "adjust", adjustInit, adjustGetFrame,
                        adjustFree, fmParallel, 0, data, core);
}

VS_EXTERNAL_API(void)
VapourSynthPluginInit(VSConfigPlugin configFunc,
                      VSRegisterFunction registerFunc, VSPlugin *plugin) {
    configFunc("xyz.noctem.adjust", "adjust", "Filter for adjusting",
               VAPOURSYNTH_API_VERSION, 1, plugin);
    registerFunc("adjust", "clip:clip;sat:float:opt;hue:float:opt;luma:float[]:opt", adjustCreate, NULL, plugin);
}
