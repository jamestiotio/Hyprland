#include "Renderer.hpp"
#include "../Compositor.hpp"
#include "linux-dmabuf-unstable-v1-protocol.h"
#include "../helpers/Region.hpp"

CHyprRenderer::CHyprRenderer() {
    const auto ENV = getenv("WLR_DRM_NO_ATOMIC");

    if (ENV && std::string(ENV) == "1")
        m_bTearingEnvSatisfied = true;
}

void renderSurface(struct wlr_surface* surface, int x, int y, void* data) {
    const auto TEXTURE = wlr_surface_get_texture(surface);
    const auto RDATA   = (SRenderData*)data;

    if (!TEXTURE)
        return;

    TRACY_GPU_ZONE("RenderSurface");

    double outputX = 0, outputY = 0;
    wlr_output_layout_output_coords(g_pCompositor->m_sWLROutputLayout, RDATA->pMonitor->output, &outputX, &outputY);

    wlr_box windowBox;
    if (RDATA->surface && surface == RDATA->surface)
        windowBox = {(int)outputX + RDATA->x + x, (int)outputY + RDATA->y + y, RDATA->w, RDATA->h};
    else { //  here we clamp to 2, these might be some tiny specks
        windowBox = {(int)outputX + RDATA->x + x, (int)outputY + RDATA->y + y, std::max(surface->current.width, 2), std::max(surface->current.height, 2)};
        if (RDATA->pWindow && RDATA->pWindow->m_vRealSize.isBeingAnimated() && RDATA->surface && RDATA->surface != surface && RDATA->squishOversized /* subsurface */) {
            // adjust subsurfaces to the window
            windowBox.width  = (windowBox.width / RDATA->pWindow->m_vReportedSize.x) * RDATA->pWindow->m_vRealSize.vec().x;
            windowBox.height = (windowBox.height / RDATA->pWindow->m_vReportedSize.y) * RDATA->pWindow->m_vRealSize.vec().y;
        }
    }

    if (RDATA->squishOversized) {
        if (x + windowBox.width > RDATA->w)
            windowBox.width = RDATA->w - x;
        if (y + windowBox.height > RDATA->h)
            windowBox.height = RDATA->h - y;
    }

    g_pHyprRenderer->calculateUVForSurface(RDATA->pWindow, surface, RDATA->squishOversized);

    scaleBox(&windowBox, RDATA->pMonitor->scale);

    float rounding = RDATA->rounding;

    rounding -= 1; // to fix a border issue

    if (RDATA->dontRound)
        rounding = 0;

    const bool CANDISABLEBLEND = RDATA->alpha >= 1.f && rounding == 0 && surface->opaque;

    if (CANDISABLEBLEND)
        g_pHyprOpenGL->blend(false);
    else
        g_pHyprOpenGL->blend(true);

    if (RDATA->surface && surface == RDATA->surface) {
        if (wlr_xwayland_surface_try_from_wlr_surface(surface) && !wlr_xwayland_surface_try_from_wlr_surface(surface)->has_alpha && RDATA->fadeAlpha * RDATA->alpha == 1.f) {
            g_pHyprOpenGL->renderTexture(TEXTURE, &windowBox, RDATA->fadeAlpha * RDATA->alpha, rounding, true);
        } else {
            if (RDATA->blur)
                g_pHyprOpenGL->renderTextureWithBlur(TEXTURE, &windowBox, RDATA->fadeAlpha * RDATA->alpha, surface, rounding, RDATA->blockBlurOptimization, RDATA->fadeAlpha);
            else
                g_pHyprOpenGL->renderTexture(TEXTURE, &windowBox, RDATA->fadeAlpha * RDATA->alpha, rounding, true);
        }
    } else {
        g_pHyprOpenGL->renderTexture(TEXTURE, &windowBox, RDATA->fadeAlpha * RDATA->alpha, rounding, true);
    }

    if (!g_pHyprRenderer->m_bBlockSurfaceFeedback) {
        wlr_surface_send_frame_done(surface, RDATA->when);
        wlr_presentation_surface_textured_on_output(g_pCompositor->m_sWLRPresentation, surface, RDATA->pMonitor->output);
    }

    g_pHyprOpenGL->blend(true);

    // reset the UV, we might've set it above
    g_pHyprOpenGL->m_RenderData.primarySurfaceUVTopLeft     = Vector2D(-1, -1);
    g_pHyprOpenGL->m_RenderData.primarySurfaceUVBottomRight = Vector2D(-1, -1);
}

bool CHyprRenderer::shouldRenderWindow(CWindow* pWindow, CMonitor* pMonitor, CWorkspace* pWorkspace) {
    wlr_box geometry = pWindow->getFullWindowBoundingBox();

    if (!wlr_output_layout_intersects(g_pCompositor->m_sWLROutputLayout, pMonitor->output, &geometry))
        return false;

    if (pWindow->m_iWorkspaceID == -1)
        return false;

    if (pWindow->m_bPinned)
        return true;

    const auto PWINDOWWORKSPACE = g_pCompositor->getWorkspaceByID(pWindow->m_iWorkspaceID);
    if (PWINDOWWORKSPACE && PWINDOWWORKSPACE->m_iMonitorID == pMonitor->ID) {
        if (PWINDOWWORKSPACE->m_vRenderOffset.isBeingAnimated() || PWINDOWWORKSPACE->m_fAlpha.isBeingAnimated() || PWINDOWWORKSPACE->m_bForceRendering) {
            return true;
        } else {
            if (!(!PWINDOWWORKSPACE->m_bHasFullscreenWindow || pWindow->m_bIsFullscreen || (pWindow->m_bIsFloating && pWindow->m_bCreatedOverFullscreen)))
                return false;
        }
    }

    if (pWindow->m_iWorkspaceID == pWorkspace->m_iID)
        return true;

    // if not, check if it maybe is active on a different monitor.
    if (g_pCompositor->isWorkspaceVisible(pWindow->m_iWorkspaceID) && pWindow->m_bIsFloating /* tiled windows can't be multi-ws */)
        return !pWindow->m_bIsFullscreen; // Do not draw fullscreen windows on other monitors

    if (pMonitor->specialWorkspaceID == pWindow->m_iWorkspaceID)
        return true;

    return false;
}

bool CHyprRenderer::shouldRenderWindow(CWindow* pWindow) {

    if (!g_pCompositor->windowValidMapped(pWindow))
        return false;

    const auto PWORKSPACE = g_pCompositor->getWorkspaceByID(pWindow->m_iWorkspaceID);

    if (pWindow->m_iWorkspaceID == -1)
        return false;

    if (pWindow->m_bPinned || PWORKSPACE->m_bForceRendering)
        return true;

    if (g_pCompositor->isWorkspaceVisible(pWindow->m_iWorkspaceID))
        return true;

    for (auto& m : g_pCompositor->m_vMonitors) {
        if (PWORKSPACE && PWORKSPACE->m_iMonitorID == m->ID && (PWORKSPACE->m_vRenderOffset.isBeingAnimated() || PWORKSPACE->m_fAlpha.isBeingAnimated()))
            return true;

        if (m->specialWorkspaceID && g_pCompositor->isWorkspaceSpecial(pWindow->m_iWorkspaceID))
            return true;
    }

    return false;
}

void CHyprRenderer::renderWorkspaceWindowsFullscreen(CMonitor* pMonitor, CWorkspace* pWorkspace, timespec* time) {
    CWindow* pWorkspaceWindow = nullptr;

    EMIT_HOOK_EVENT("render", RENDER_PRE_WINDOWS);

    // loop over the tiled windows that are fading out
    for (auto& w : g_pCompositor->m_vWindows) {
        if (w->m_iWorkspaceID != pMonitor->activeWorkspace)
            continue;

        if (w->m_fAlpha.fl() == 0.f)
            continue;

        if (w->m_bIsFullscreen || w->m_bIsFloating)
            continue;

        renderWindow(w.get(), pMonitor, time, true, RENDER_PASS_ALL);
    }

    // and floating ones too
    for (auto& w : g_pCompositor->m_vWindows) {
        if (w->m_iWorkspaceID != pMonitor->activeWorkspace)
            continue;

        if (w->m_fAlpha.fl() == 0.f)
            continue;

        if (w->m_bIsFullscreen || !w->m_bIsFloating)
            continue;

        renderWindow(w.get(), pMonitor, time, true, RENDER_PASS_ALL);
    }

    for (auto& w : g_pCompositor->m_vWindows) {
        const auto PWORKSPACE = g_pCompositor->getWorkspaceByID(w->m_iWorkspaceID);

        if (w->m_iWorkspaceID != pWorkspace->m_iID || !w->m_bIsFullscreen) {
            if (!(PWORKSPACE && (PWORKSPACE->m_vRenderOffset.isBeingAnimated() || PWORKSPACE->m_fAlpha.isBeingAnimated() || PWORKSPACE->m_bForceRendering)))
                continue;

            if (w->m_iMonitorID != pMonitor->ID)
                continue;
        }

        if (w->m_iWorkspaceID == pMonitor->activeWorkspace && !w->m_bIsFullscreen)
            continue;

        renderWindow(w.get(), pMonitor, time, pWorkspace->m_efFullscreenMode != FULLSCREEN_FULL, RENDER_PASS_ALL);

        pWorkspaceWindow = w.get();
    }

    if (!pWorkspaceWindow) {
        // ?? happens sometimes...
        pWorkspace->m_bHasFullscreenWindow = false;
        return; // this will produce one blank frame. Oh well.
    }

    // then render windows over fullscreen.
    for (auto& w : g_pCompositor->m_vWindows) {
        if (w->m_iWorkspaceID != pWorkspaceWindow->m_iWorkspaceID || (!w->m_bCreatedOverFullscreen && !w->m_bPinned) || (!w->m_bIsMapped && !w->m_bFadingOut) || w->m_bIsFullscreen)
            continue;

        renderWindow(w.get(), pMonitor, time, true, RENDER_PASS_ALL);
    }
}

void CHyprRenderer::renderWorkspaceWindows(CMonitor* pMonitor, CWorkspace* pWorkspace, timespec* time) {
    CWindow* lastWindow = nullptr;

    EMIT_HOOK_EVENT("render", RENDER_PRE_WINDOWS);

    // Non-floating main
    for (auto& w : g_pCompositor->m_vWindows) {
        if (w->isHidden() && !w->m_bIsMapped && !w->m_bFadingOut)
            continue;

        if (w->m_bIsFloating)
            continue; // floating are in the second pass

        if (g_pCompositor->isWorkspaceSpecial(w->m_iWorkspaceID))
            continue; // special are in the third pass

        if (!shouldRenderWindow(w.get(), pMonitor, pWorkspace))
            continue;

        // render active window after all others of this pass
        if (w.get() == g_pCompositor->m_pLastWindow) {
            lastWindow = w.get();
            continue;
        }

        // render the bad boy
        renderWindow(w.get(), pMonitor, time, true, RENDER_PASS_MAIN);
    }

    if (lastWindow)
        renderWindow(lastWindow, pMonitor, time, true, RENDER_PASS_MAIN);

    // Non-floating popup
    for (auto& w : g_pCompositor->m_vWindows) {
        if (w->isHidden() && !w->m_bIsMapped && !w->m_bFadingOut)
            continue;

        if (w->m_bIsFloating)
            continue; // floating are in the second pass

        if (g_pCompositor->isWorkspaceSpecial(w->m_iWorkspaceID))
            continue; // special are in the third pass

        if (!shouldRenderWindow(w.get(), pMonitor, pWorkspace))
            continue;

        // render the bad boy
        renderWindow(w.get(), pMonitor, time, true, RENDER_PASS_POPUP);
    }

    // floating on top
    for (auto& w : g_pCompositor->m_vWindows) {
        if (w->isHidden() && !w->m_bIsMapped && !w->m_bFadingOut)
            continue;

        if (!w->m_bIsFloating || w->m_bPinned)
            continue;

        if (g_pCompositor->isWorkspaceSpecial(w->m_iWorkspaceID))
            continue;

        if (!shouldRenderWindow(w.get(), pMonitor, pWorkspace))
            continue;

        // render the bad boy
        renderWindow(w.get(), pMonitor, time, true, RENDER_PASS_ALL);
    }

    // pinned always above
    for (auto& w : g_pCompositor->m_vWindows) {
        if (w->isHidden() && !w->m_bIsMapped && !w->m_bFadingOut)
            continue;

        if (!w->m_bPinned || !w->m_bIsFloating)
            continue;

        if (g_pCompositor->isWorkspaceSpecial(w->m_iWorkspaceID))
            continue;

        if (!shouldRenderWindow(w.get(), pMonitor, pWorkspace))
            continue;

        // render the bad boy
        renderWindow(w.get(), pMonitor, time, true, RENDER_PASS_ALL);
    }
}

void CHyprRenderer::renderWindow(CWindow* pWindow, CMonitor* pMonitor, timespec* time, bool decorate, eRenderPassMode mode, bool ignorePosition, bool ignoreAllGeometry) {
    if (pWindow->isHidden())
        return;

    if (pWindow->m_bFadingOut) {
        if (pMonitor->ID == pWindow->m_iMonitorID) // TODO: fix this
            g_pHyprOpenGL->renderSnapshot(&pWindow);
        return;
    }

    TRACY_GPU_ZONE("RenderWindow");

    const auto         PWORKSPACE  = g_pCompositor->getWorkspaceByID(pWindow->m_iWorkspaceID);
    const auto         REALPOS     = pWindow->m_vRealPosition.vec() + (pWindow->m_bPinned ? Vector2D{} : PWORKSPACE->m_vRenderOffset.vec());
    static auto* const PDIMAROUND  = &g_pConfigManager->getConfigValuePtr("decoration:dim_around")->floatValue;
    static auto* const PBORDERSIZE = &g_pConfigManager->getConfigValuePtr("general:border_size")->intValue;

    SRenderData        renderdata = {pMonitor, time, REALPOS.x, REALPOS.y};
    if (ignorePosition) {
        renderdata.x = pMonitor->vecPosition.x;
        renderdata.y = pMonitor->vecPosition.y;
    }

    if (ignoreAllGeometry)
        decorate = false;

    renderdata.surface   = pWindow->m_pWLSurface.wlr();
    renderdata.w         = std::max(pWindow->m_vRealSize.vec().x, 5.0); // clamp the size to min 5,
    renderdata.h         = std::max(pWindow->m_vRealSize.vec().y, 5.0); // otherwise we'll have issues later with invalid boxes
    renderdata.dontRound = (pWindow->m_bIsFullscreen && PWORKSPACE->m_efFullscreenMode == FULLSCREEN_FULL) || (!pWindow->m_sSpecialRenderData.rounding);
    renderdata.fadeAlpha = pWindow->m_fAlpha.fl() * (pWindow->m_bPinned ? 1.f : PWORKSPACE->m_fAlpha.fl());
    renderdata.alpha     = pWindow->m_fActiveInactiveAlpha.fl();
    renderdata.decorate  = decorate && !pWindow->m_bX11DoesntWantBorders && (!pWindow->m_bIsFullscreen || PWORKSPACE->m_efFullscreenMode != FULLSCREEN_FULL);
    renderdata.rounding  = ignoreAllGeometry || renderdata.dontRound ? 0 : pWindow->rounding() * pMonitor->scale;
    renderdata.blur      = !ignoreAllGeometry; // if it shouldn't, it will be ignored later
    renderdata.pWindow   = pWindow;

    if (ignoreAllGeometry) {
        renderdata.alpha     = 1.f;
        renderdata.fadeAlpha = 1.f;
    }

    // apply opaque
    if (pWindow->m_sAdditionalConfigData.forceOpaque)
        renderdata.alpha = 1.f;

    g_pHyprOpenGL->m_pCurrentWindow = pWindow;

    EMIT_HOOK_EVENT("render", RENDER_PRE_WINDOW);

    if (*PDIMAROUND && pWindow->m_sAdditionalConfigData.dimAround && !m_bRenderingSnapshot && mode != RENDER_PASS_POPUP) {
        wlr_box monbox = {0, 0, g_pHyprOpenGL->m_RenderData.pMonitor->vecTransformedSize.x, g_pHyprOpenGL->m_RenderData.pMonitor->vecTransformedSize.y};
        g_pHyprOpenGL->renderRect(&monbox, CColor(0, 0, 0, *PDIMAROUND * renderdata.alpha * renderdata.fadeAlpha));
    }

    // clip box for animated offsets
    Vector2D offset;
    if (!ignorePosition && pWindow->m_bIsFloating && !pWindow->m_bPinned) {
        if (PWORKSPACE->m_vRenderOffset.vec().x != 0) {
            const auto PWSMON   = g_pCompositor->getMonitorFromID(PWORKSPACE->m_iMonitorID);
            const auto PROGRESS = PWORKSPACE->m_vRenderOffset.vec().x / PWSMON->vecSize.x;
            const auto WINBB    = pWindow->getFullWindowBoundingBox();

            if (WINBB.x < PWSMON->vecPosition.x) {
                offset.x = (PWSMON->vecPosition.x - WINBB.x) * PROGRESS;
            } else if (WINBB.x + WINBB.width > PWSMON->vecPosition.x + PWSMON->vecSize.x) {
                offset.x = (WINBB.x + WINBB.width - PWSMON->vecPosition.x - PWSMON->vecSize.x) * PROGRESS;
            }
        } else if (PWORKSPACE->m_vRenderOffset.vec().y) {
            const auto PWSMON   = g_pCompositor->getMonitorFromID(PWORKSPACE->m_iMonitorID);
            const auto PROGRESS = PWORKSPACE->m_vRenderOffset.vec().y / PWSMON->vecSize.y;
            const auto WINBB    = pWindow->getFullWindowBoundingBox();

            if (WINBB.y < PWSMON->vecPosition.y) {
                offset.y = (PWSMON->vecPosition.y - WINBB.y) * PROGRESS;
            } else if (WINBB.y + WINBB.height > PWSMON->vecPosition.y + PWSMON->vecSize.y) {
                offset.y = (WINBB.y + WINBB.width - PWSMON->vecPosition.y - PWSMON->vecSize.y) * PROGRESS;
            }
        }

        renderdata.x += offset.x;
        renderdata.y += offset.y;
    }

    // render window decorations first, if not fullscreen full
    if (mode == RENDER_PASS_ALL || mode == RENDER_PASS_MAIN) {
        if (!pWindow->m_bIsFullscreen || PWORKSPACE->m_efFullscreenMode != FULLSCREEN_FULL)
            for (auto& wd : pWindow->m_dWindowDecorations)
                wd->draw(pMonitor, renderdata.alpha * renderdata.fadeAlpha, offset);

        static auto* const PXWLUSENN = &g_pConfigManager->getConfigValuePtr("xwayland:use_nearest_neighbor")->intValue;
        if (pWindow->m_bIsX11 && *PXWLUSENN)
            g_pHyprOpenGL->m_RenderData.useNearestNeighbor = true;

        wlr_surface_for_each_surface(pWindow->m_pWLSurface.wlr(), renderSurface, &renderdata);

        g_pHyprOpenGL->m_RenderData.useNearestNeighbor = false;

        if (renderdata.decorate && pWindow->m_sSpecialRenderData.border) {
            auto       grad     = g_pHyprOpenGL->m_pCurrentWindow->m_cRealBorderColor;
            const bool ANIMATED = g_pHyprOpenGL->m_pCurrentWindow->m_fBorderFadeAnimationProgress.isBeingAnimated();
            float      a1       = renderdata.fadeAlpha * renderdata.alpha * (ANIMATED ? g_pHyprOpenGL->m_pCurrentWindow->m_fBorderFadeAnimationProgress.fl() : 1.f);

            if (g_pHyprOpenGL->m_pCurrentWindow->m_fBorderAngleAnimationProgress.getConfig()->pValues->internalEnabled) {
                grad.m_fAngle += g_pHyprOpenGL->m_pCurrentWindow->m_fBorderAngleAnimationProgress.fl() * M_PI * 2;
                grad.m_fAngle = normalizeAngleRad(grad.m_fAngle);
            }

            wlr_box windowBox = {renderdata.x - pMonitor->vecPosition.x, renderdata.y - pMonitor->vecPosition.y, renderdata.w, renderdata.h};

            scaleBox(&windowBox, pMonitor->scale);

            int borderSize = pWindow->m_sSpecialRenderData.borderSize.toUnderlying() == -1 ? *PBORDERSIZE : pWindow->m_sSpecialRenderData.borderSize.toUnderlying();
            if (pWindow->m_sAdditionalConfigData.borderSize.toUnderlying() != -1)
                borderSize = pWindow->m_sAdditionalConfigData.borderSize.toUnderlying();

            g_pHyprOpenGL->renderBorder(&windowBox, grad, renderdata.rounding, borderSize, a1);

            if (ANIMATED) {
                float a2 = renderdata.fadeAlpha * renderdata.alpha * (1.f - g_pHyprOpenGL->m_pCurrentWindow->m_fBorderFadeAnimationProgress.fl());
                g_pHyprOpenGL->renderBorder(&windowBox, g_pHyprOpenGL->m_pCurrentWindow->m_cRealBorderColorPrevious, renderdata.rounding, borderSize, a2);
            }
        }
    }

    if (mode == RENDER_PASS_ALL || mode == RENDER_PASS_POPUP) {
        if (!pWindow->m_bIsX11) {
            wlr_box geom;
            wlr_xdg_surface_get_geometry(pWindow->m_uSurface.xdg, &geom);

            renderdata.x -= geom.x;
            renderdata.y -= geom.y;

            renderdata.dontRound       = true; // don't round popups
            renderdata.pMonitor        = pMonitor;
            renderdata.squishOversized = false; // don't squish popups
            wlr_xdg_surface_for_each_popup_surface(pWindow->m_uSurface.xdg, renderSurface, &renderdata);
        }
    }

    EMIT_HOOK_EVENT("render", RENDER_POST_WINDOW);

    g_pHyprOpenGL->m_pCurrentWindow     = nullptr;
    g_pHyprOpenGL->m_RenderData.clipBox = {0, 0, 0, 0};
}

void CHyprRenderer::renderLayer(SLayerSurface* pLayer, CMonitor* pMonitor, timespec* time) {
    if (pLayer->fadingOut) {
        g_pHyprOpenGL->renderSnapshot(&pLayer);
        return;
    }

    TRACY_GPU_ZONE("RenderLayer");

    SRenderData renderdata           = {pMonitor, time, pLayer->geometry.x, pLayer->geometry.y};
    renderdata.fadeAlpha             = pLayer->alpha.fl();
    renderdata.blur                  = pLayer->forceBlur;
    renderdata.surface               = pLayer->layerSurface->surface;
    renderdata.decorate              = false;
    renderdata.w                     = pLayer->geometry.width;
    renderdata.h                     = pLayer->geometry.height;
    renderdata.blockBlurOptimization = pLayer->layer == ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM || pLayer->layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;

    g_pHyprOpenGL->m_pCurrentLayer = pLayer;

    if (pLayer->ignoreAlpha) {
        g_pHyprOpenGL->m_RenderData.discardMode |= DISCARD_ALPHA;
        g_pHyprOpenGL->m_RenderData.discardOpacity = pLayer->ignoreAlphaValue;
    }
    wlr_surface_for_each_surface(pLayer->layerSurface->surface, renderSurface, &renderdata);
    g_pHyprOpenGL->m_RenderData.discardMode &= ~DISCARD_ALPHA;

    renderdata.squishOversized = false; // don't squish popups
    renderdata.dontRound       = true;
    wlr_layer_surface_v1_for_each_popup_surface(pLayer->layerSurface, renderSurface, &renderdata);

    g_pHyprOpenGL->m_pCurrentLayer = nullptr;
}

void CHyprRenderer::renderIMEPopup(SIMEPopup* pPopup, CMonitor* pMonitor, timespec* time) {
    SRenderData renderdata = {pMonitor, time, pPopup->realX, pPopup->realY};

    renderdata.blur     = false;
    renderdata.surface  = pPopup->pSurface->surface;
    renderdata.decorate = false;
    renderdata.w        = pPopup->pSurface->surface->current.width;
    renderdata.h        = pPopup->pSurface->surface->current.height;

    wlr_surface_for_each_surface(pPopup->pSurface->surface, renderSurface, &renderdata);
}

void CHyprRenderer::renderSessionLockSurface(SSessionLockSurface* pSurface, CMonitor* pMonitor, timespec* time) {
    SRenderData renderdata = {pMonitor, time, pMonitor->vecPosition.x, pMonitor->vecPosition.y};

    renderdata.blur     = false;
    renderdata.surface  = pSurface->pWlrLockSurface->surface;
    renderdata.decorate = false;
    renderdata.w        = pMonitor->vecSize.x;
    renderdata.h        = pMonitor->vecSize.y;

    wlr_surface_for_each_surface(pSurface->pWlrLockSurface->surface, renderSurface, &renderdata);
}

void CHyprRenderer::renderAllClientsForWorkspace(CMonitor* pMonitor, CWorkspace* pWorkspace, timespec* time, const Vector2D& translate, const float& scale) {
    static auto* const     PDIMSPECIAL  = &g_pConfigManager->getConfigValuePtr("decoration:dim_special")->floatValue;
    static auto* const     PBLURSPECIAL = &g_pConfigManager->getConfigValuePtr("decoration:blur:special")->intValue;
    static auto* const     PBLUR        = &g_pConfigManager->getConfigValuePtr("decoration:blur:enabled")->intValue;

    const SRenderModifData RENDERMODIFDATA = {translate, scale};

    if (!pMonitor)
        return;

    if (!g_pCompositor->m_sSeat.exclusiveClient && g_pSessionLockManager->isSessionLocked()) {
        // locked with no exclusive, draw only red
        wlr_box boxe = {0, 0, INT16_MAX, INT16_MAX};
        g_pHyprOpenGL->renderRect(&boxe, CColor(1.0, 0.2, 0.2, 1.0));
        return;
    }

    // todo: matrices are buggy atm for some reason, but probably would be preferable in the long run
    // g_pHyprOpenGL->saveMatrix();
    // g_pHyprOpenGL->setMatrixScaleTranslate(translate, scale);
    g_pHyprOpenGL->m_RenderData.renderModif = RENDERMODIFDATA;

    // for storing damage when we optimize for occlusion
    CRegion preOccludedDamage{g_pHyprOpenGL->m_RenderData.damage};

    // Render layer surfaces below windows for monitor
    // if we have a fullscreen, opaque window that convers the screen, we can skip this.
    // TODO: check better with solitary after MR for tearing.
    const auto PFULLWINDOW = g_pCompositor->getFullscreenWindowOnWorkspace(pWorkspace->m_iID);
    if (!pWorkspace->m_bHasFullscreenWindow || pWorkspace->m_efFullscreenMode != FULLSCREEN_FULL || !PFULLWINDOW || PFULLWINDOW->m_vRealSize.isBeingAnimated() ||
        !PFULLWINDOW->opaque() || pWorkspace->m_vRenderOffset.vec() != Vector2D{}) {

        if (!g_pHyprOpenGL->m_RenderData.pCurrentMonData->blurFBShouldRender)
            setOccludedForBackLayers(g_pHyprOpenGL->m_RenderData.damage, pWorkspace);

        for (auto& ls : pMonitor->m_aLayerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND]) {
            renderLayer(ls.get(), pMonitor, time);
        }
        for (auto& ls : pMonitor->m_aLayerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM]) {
            renderLayer(ls.get(), pMonitor, time);
        }

        g_pHyprOpenGL->m_RenderData.damage = preOccludedDamage;
    }

    // pre window pass
    g_pHyprOpenGL->preWindowPass();

    if (pWorkspace->m_bHasFullscreenWindow)
        renderWorkspaceWindowsFullscreen(pMonitor, pWorkspace, time);
    else
        renderWorkspaceWindows(pMonitor, pWorkspace, time);

    g_pHyprOpenGL->m_RenderData.renderModif = {};

    // and then special
    for (auto& ws : g_pCompositor->m_vWorkspaces) {
        if (ws->m_iMonitorID == pMonitor->ID && ws->m_fAlpha.fl() > 0.f && ws->m_bIsSpecialWorkspace) {
            const auto SPECIALANIMPROGRS = ws->m_vRenderOffset.isBeingAnimated() ? ws->m_vRenderOffset.getCurveValue() : ws->m_fAlpha.getCurveValue();
            const bool ANIMOUT           = !pMonitor->specialWorkspaceID;

            if (*PDIMSPECIAL != 0.f) {
                wlr_box monbox = {translate.x, translate.y, pMonitor->vecTransformedSize.x * scale, pMonitor->vecTransformedSize.y * scale};
                g_pHyprOpenGL->renderRect(&monbox, CColor(0, 0, 0, *PDIMSPECIAL * (ANIMOUT ? (1.0 - SPECIALANIMPROGRS) : SPECIALANIMPROGRS)));
            }

            if (*PBLURSPECIAL && *PBLUR) {
                wlr_box monbox = {translate.x, translate.y, pMonitor->vecTransformedSize.x * scale, pMonitor->vecTransformedSize.y * scale};
                g_pHyprOpenGL->renderRectWithBlur(&monbox, CColor(0, 0, 0, 0), 0, (ANIMOUT ? (1.0 - SPECIALANIMPROGRS) : SPECIALANIMPROGRS));
            }

            break;
        }
    }

    for (auto& w : g_pCompositor->m_vWindows) {
        if (w->isHidden() && !w->m_bIsMapped && !w->m_bFadingOut)
            continue;

        if (!g_pCompositor->isWorkspaceSpecial(w->m_iWorkspaceID))
            continue;

        if (!shouldRenderWindow(w.get(), pMonitor, pWorkspace))
            continue;

        // render the bad boy
        renderWindow(w.get(), pMonitor, time, true, RENDER_PASS_ALL);
    }

    EMIT_HOOK_EVENT("render", RENDER_POST_WINDOWS);

    // Render surfaces above windows for monitor
    for (auto& ls : pMonitor->m_aLayerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP]) {
        renderLayer(ls.get(), pMonitor, time);
    }

    // Render IME popups
    for (auto& imep : g_pInputManager->m_sIMERelay.m_lIMEPopups) {
        renderIMEPopup(&imep, pMonitor, time);
    }

    for (auto& ls : pMonitor->m_aLayerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY]) {
        renderLayer(ls.get(), pMonitor, time);
    }

    renderDragIcon(pMonitor, time);

    //g_pHyprOpenGL->restoreMatrix();
    g_pHyprOpenGL->m_RenderData.renderModif = {};
}

void CHyprRenderer::renderLockscreen(CMonitor* pMonitor, timespec* now) {
    TRACY_GPU_ZONE("RenderLockscreen");

    if (g_pSessionLockManager->isSessionLocked()) {
        const auto PSLS = g_pSessionLockManager->getSessionLockSurfaceForMonitor(pMonitor->ID);

        if (!PSLS) {
            // locked with no surface, fill with red
            wlr_box boxe = {0, 0, INT16_MAX, INT16_MAX};
            g_pHyprOpenGL->renderRect(&boxe, CColor(1.0, 0.2, 0.2, 1.0));
        } else {
            renderSessionLockSurface(PSLS, pMonitor, now);
        }
    }
}

void CHyprRenderer::calculateUVForSurface(CWindow* pWindow, wlr_surface* pSurface, bool main) {
    if (!pWindow || !pWindow->m_bIsX11) {
        Vector2D uvTL;
        Vector2D uvBR = Vector2D(1, 1);

        if (pSurface->current.viewport.has_src) {
            // we stretch it to dest. if no dest, to 1,1
            wlr_fbox bufferSource;
            wlr_surface_get_buffer_source_box(pSurface, &bufferSource);

            Vector2D bufferSize = Vector2D(pSurface->buffer->texture->width, pSurface->buffer->texture->height);

            // calculate UV for the basic src_box. Assume dest == size. Scale to dest later
            uvTL = Vector2D(bufferSource.x / bufferSize.x, bufferSource.y / bufferSize.y);
            uvBR = Vector2D((bufferSource.x + bufferSource.width) / bufferSize.x, (bufferSource.y + bufferSource.height) / bufferSize.y);

            if (uvBR.x < 0.01f || uvBR.y < 0.01f) {
                uvTL = Vector2D();
                uvBR = Vector2D(1, 1);
            }
        }

        g_pHyprOpenGL->m_RenderData.primarySurfaceUVTopLeft     = uvTL;
        g_pHyprOpenGL->m_RenderData.primarySurfaceUVBottomRight = uvBR;

        if (g_pHyprOpenGL->m_RenderData.primarySurfaceUVTopLeft == Vector2D() && g_pHyprOpenGL->m_RenderData.primarySurfaceUVBottomRight == Vector2D(1, 1)) {
            // No special UV mods needed
            g_pHyprOpenGL->m_RenderData.primarySurfaceUVTopLeft     = Vector2D(-1, -1);
            g_pHyprOpenGL->m_RenderData.primarySurfaceUVBottomRight = Vector2D(-1, -1);
        }

        if (!main || !pWindow)
            return;

        wlr_box geom;
        wlr_xdg_surface_get_geometry(pWindow->m_uSurface.xdg, &geom);

        // ignore X and Y, adjust uv
        if (geom.x != 0 || geom.y != 0 || geom.width > pWindow->m_vRealSize.goalv().x || geom.height > pWindow->m_vRealSize.goalv().y) {
            const auto XPERC = (double)geom.x / (double)pSurface->current.width;
            const auto YPERC = (double)geom.y / (double)pSurface->current.height;
            const auto WPERC = (double)(geom.x + geom.width) / (double)pSurface->current.width;
            const auto HPERC = (double)(geom.y + geom.height) / (double)pSurface->current.height;

            const auto TOADDTL = Vector2D(XPERC * (uvBR.x - uvTL.x), YPERC * (uvBR.y - uvTL.y));
            uvBR               = uvBR - Vector2D(1.0 - WPERC * (uvBR.x - uvTL.x), 1.0 - HPERC * (uvBR.y - uvTL.y));
            uvTL               = uvTL + TOADDTL;

            if (geom.width > pWindow->m_vRealSize.goalv().x || geom.height > pWindow->m_vRealSize.goalv().y) {
                uvBR.x = uvBR.x * (pWindow->m_vRealSize.goalv().x / geom.width);
                uvBR.y = uvBR.y * (pWindow->m_vRealSize.goalv().y / geom.height);
            }
        }

        g_pHyprOpenGL->m_RenderData.primarySurfaceUVTopLeft     = uvTL;
        g_pHyprOpenGL->m_RenderData.primarySurfaceUVBottomRight = uvBR;

        if (g_pHyprOpenGL->m_RenderData.primarySurfaceUVTopLeft == Vector2D() && g_pHyprOpenGL->m_RenderData.primarySurfaceUVBottomRight == Vector2D(1, 1)) {
            // No special UV mods needed
            g_pHyprOpenGL->m_RenderData.primarySurfaceUVTopLeft     = Vector2D(-1, -1);
            g_pHyprOpenGL->m_RenderData.primarySurfaceUVBottomRight = Vector2D(-1, -1);
        }
    } else {
        g_pHyprOpenGL->m_RenderData.primarySurfaceUVTopLeft     = Vector2D(-1, -1);
        g_pHyprOpenGL->m_RenderData.primarySurfaceUVBottomRight = Vector2D(-1, -1);
    }
}

void countSubsurfacesIter(wlr_surface* pSurface, int x, int y, void* data) {
    *(int*)data += 1;
}

bool CHyprRenderer::attemptDirectScanout(CMonitor* pMonitor) {
    if (!pMonitor->mirrors.empty() || pMonitor->isMirror() || m_bDirectScanoutBlocked)
        return false; // do not DS if this monitor is being mirrored. Will break the functionality.

    if (!wlr_output_is_direct_scanout_allowed(pMonitor->output))
        return false;

    const auto PCANDIDATE = pMonitor->solitaryClient;

    if (!PCANDIDATE)
        return false;

    const auto PSURFACE = g_pXWaylandManager->getWindowSurface(PCANDIDATE);

    if (!PSURFACE || PSURFACE->current.scale != pMonitor->output->scale || PSURFACE->current.transform != pMonitor->output->transform)
        return false;

    // finally, we should be GTG.
    wlr_output_attach_buffer(pMonitor->output, &PSURFACE->buffer->base);

    if (!wlr_output_test(pMonitor->output))
        return false;

    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_surface_send_frame_done(PSURFACE, &now);
    wlr_presentation_surface_scanned_out_on_output(g_pCompositor->m_sWLRPresentation, PSURFACE, pMonitor->output);

    if (wlr_output_commit(pMonitor->output)) {
        if (!m_pLastScanout) {
            m_pLastScanout = PCANDIDATE;
            Debug::log(LOG, "Entered a direct scanout to {:x}: \"{}\"", (uintptr_t)PCANDIDATE, PCANDIDATE->m_szTitle);
        }
    } else {
        m_pLastScanout = nullptr;
        return false;
    }

    return true;
}

void CHyprRenderer::renderMonitor(CMonitor* pMonitor) {
    static std::chrono::high_resolution_clock::time_point startRender        = std::chrono::high_resolution_clock::now();
    static std::chrono::high_resolution_clock::time_point startRenderOverlay = std::chrono::high_resolution_clock::now();
    static std::chrono::high_resolution_clock::time_point endRenderOverlay   = std::chrono::high_resolution_clock::now();

    static auto* const                                    PDEBUGOVERLAY       = &g_pConfigManager->getConfigValuePtr("debug:overlay")->intValue;
    static auto* const                                    PDAMAGETRACKINGMODE = &g_pConfigManager->getConfigValuePtr("debug:damage_tracking")->intValue;
    static auto* const                                    PDAMAGEBLINK        = &g_pConfigManager->getConfigValuePtr("debug:damage_blink")->intValue;
    static auto* const                                    PNODIRECTSCANOUT    = &g_pConfigManager->getConfigValuePtr("misc:no_direct_scanout")->intValue;
    static auto* const                                    PVFR                = &g_pConfigManager->getConfigValuePtr("misc:vfr")->intValue;
    static auto* const                                    PZOOMFACTOR         = &g_pConfigManager->getConfigValuePtr("misc:cursor_zoom_factor")->floatValue;
    static auto* const                                    PRENDERTEX          = &g_pConfigManager->getConfigValuePtr("misc:disable_hyprland_logo")->intValue;
    static auto* const                                    PBACKGROUNDCOLOR    = &g_pConfigManager->getConfigValuePtr("misc:background_color")->intValue;
    static auto* const                                    PANIMENABLED        = &g_pConfigManager->getConfigValuePtr("animations:enabled")->intValue;
    static auto* const                                    PTEARINGENABLED     = &g_pConfigManager->getConfigValuePtr("general:allow_tearing")->intValue;

    static int                                            damageBlinkCleanup = 0; // because double-buffered

    if (!*PDAMAGEBLINK)
        damageBlinkCleanup = 0;

    static bool firstLaunch           = true;
    static bool firstLaunchAnimActive = true;

    float       zoomInFactorFirstLaunch = 1.f;

    if (firstLaunch) {
        firstLaunch = false;
        m_tRenderTimer.reset();
    }

    if (m_tRenderTimer.getSeconds() < 1.5f && firstLaunchAnimActive) { // TODO: make the animation system more damage-flexible so that this can be migrated to there
        if (!*PANIMENABLED) {
            zoomInFactorFirstLaunch = 1.f;
            firstLaunchAnimActive   = false;
        } else {
            zoomInFactorFirstLaunch = 2.f - g_pAnimationManager->getBezier("default")->getYForPoint(m_tRenderTimer.getSeconds() / 1.5);
            damageMonitor(pMonitor);
        }
    } else {
        firstLaunchAnimActive = false;
    }

    startRender = std::chrono::high_resolution_clock::now();

    if (*PDEBUGOVERLAY == 1) {
        g_pDebugOverlay->frameData(pMonitor);
    }

    if (pMonitor->framesToSkip > 0) {
        pMonitor->framesToSkip -= 1;

        if (!pMonitor->noFrameSchedule)
            g_pCompositor->scheduleFrameForMonitor(pMonitor);
        else {
            Debug::log(LOG, "NoFrameSchedule hit for {}.", pMonitor->szName);
        }
        g_pLayoutManager->getCurrentLayout()->recalculateMonitor(pMonitor->ID);

        if (pMonitor->framesToSkip > 10)
            pMonitor->framesToSkip = 0;
        return;
    }

    // checks //
    if (pMonitor->ID == m_pMostHzMonitor->ID ||
        *PVFR == 1) { // unfortunately with VFR we don't have the guarantee mostHz is going to be updated all the time, so we have to ignore that
        g_pCompositor->sanityCheckWorkspaces();

        g_pConfigManager->dispatchExecOnce(); // We exec-once when at least one monitor starts refreshing, meaning stuff has init'd

        if (g_pConfigManager->m_bWantsMonitorReload)
            g_pConfigManager->performMonitorReload();

        ensureCursorRenderingMode(); // so that the cursor gets hidden/shown if the user requested timeouts
    }
    //       //

    if (pMonitor->scheduledRecalc) {
        pMonitor->scheduledRecalc = false;
        g_pLayoutManager->getCurrentLayout()->recalculateMonitor(pMonitor->ID);
    }

    // gamma stuff
    if (pMonitor->gammaChanged) {
        pMonitor->gammaChanged = false;

        const auto PGAMMACTRL = wlr_gamma_control_manager_v1_get_control(g_pCompositor->m_sWLRGammaCtrlMgr, pMonitor->output);

        if (!wlr_gamma_control_v1_apply(PGAMMACTRL, &pMonitor->output->pending)) {
            Debug::log(ERR, "Could not apply gamma control to {}", pMonitor->szName);
            return;
        }

        if (!wlr_output_test(pMonitor->output)) {
            Debug::log(ERR, "Output test failed for setting gamma to {}", pMonitor->szName);
            wlr_output_rollback(pMonitor->output);
            wlr_gamma_control_v1_send_failed_and_destroy(PGAMMACTRL);
        }
    }

    // tearing and DS first
    bool shouldTear = false;
    if (pMonitor->tearingState.nextRenderTorn) {
        pMonitor->tearingState.nextRenderTorn = false;

        if (!*PTEARINGENABLED) {
            Debug::log(WARN, "Tearing commit requested but the master switch general:allow_tearing is off, ignoring");
            return;
        }

        if (g_pHyprOpenGL->m_RenderData.mouseZoomFactor != 1.0) {
            Debug::log(WARN, "Tearing commit requested but scale factor is not 1, ignoring");
            return;
        }

        if (!pMonitor->tearingState.canTear) {
            Debug::log(WARN, "Tearing commit requested but monitor doesn't support it, ignoring");
            return;
        }

        if (pMonitor->solitaryClient)
            shouldTear = true;
    }

    if (!*PNODIRECTSCANOUT && !shouldTear) {
        if (attemptDirectScanout(pMonitor)) {
            return;
        } else if (m_pLastScanout) {
            Debug::log(LOG, "Left a direct scanout.");
            m_pLastScanout = nullptr;
        }
    }

    if (pMonitor->tearingState.activelyTearing != shouldTear) {
        // change of state
        pMonitor->tearingState.activelyTearing = shouldTear;

        for (auto& m : g_pCompositor->m_vMonitors) {
            wlr_output_lock_software_cursors(m->output, pMonitor->tearingState.activelyTearing);
        }
    }

    EMIT_HOOK_EVENT("preRender", pMonitor);

    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    // check the damage
    CRegion damage;
    bool    hasChanged = pMonitor->output->needs_frame || pixman_region32_not_empty(&pMonitor->damage.current);
    int     bufferAge;

    if (!hasChanged && *PDAMAGETRACKINGMODE != DAMAGE_TRACKING_NONE && pMonitor->forceFullFrames == 0 && damageBlinkCleanup == 0)
        return;

    if (*PDAMAGETRACKINGMODE == -1) {
        Debug::log(CRIT, "Damage tracking mode -1 ????");
        return;
    }

    EMIT_HOOK_EVENT("render", RENDER_PRE);

    const bool UNLOCK_SC = g_pHyprRenderer->m_bSoftwareCursorsLocked;
    if (UNLOCK_SC)
        wlr_output_lock_software_cursors(pMonitor->output, true);

    if (!wlr_output_attach_render(pMonitor->output, &bufferAge)) {
        Debug::log(ERR, "Couldn't attach render to display {} ???", pMonitor->szName);

        if (UNLOCK_SC)
            wlr_output_lock_software_cursors(pMonitor->output, false);

        return;
    }

    wlr_damage_ring_get_buffer_damage(&pMonitor->damage, bufferAge, damage.pixman());

    pMonitor->renderingActive = true;

    // we need to cleanup fading out when rendering the appropriate context
    g_pCompositor->cleanupFadingOut(pMonitor->ID);

    // if we have no tracking or full tracking, invalidate the entire monitor
    if (*PDAMAGETRACKINGMODE == DAMAGE_TRACKING_NONE || *PDAMAGETRACKINGMODE == DAMAGE_TRACKING_MONITOR || pMonitor->forceFullFrames > 0 || damageBlinkCleanup > 0 ||
        pMonitor->isMirror() /* why??? */) {

        damage                    = {0, 0, (int)pMonitor->vecTransformedSize.x * 10, (int)pMonitor->vecTransformedSize.y * 10};
        pMonitor->lastFrameDamage = damage;
    } else {
        static auto* const PBLURENABLED = &g_pConfigManager->getConfigValuePtr("decoration:blur:enabled")->intValue;

        // if we use blur we need to expand the damage for proper blurring
        if (*PBLURENABLED == 1) {
            // TODO: can this be optimized?
            static auto* const PBLURSIZE   = &g_pConfigManager->getConfigValuePtr("decoration:blur:size")->intValue;
            static auto* const PBLURPASSES = &g_pConfigManager->getConfigValuePtr("decoration:blur:passes")->intValue;
            const auto         BLURRADIUS =
                *PBLURPASSES > 10 ? pow(2, 15) : std::clamp(*PBLURSIZE, (int64_t)1, (int64_t)40) * pow(2, *PBLURPASSES); // is this 2^pass? I don't know but it works... I think.

            // now, prep the damage, get the extended damage region
            wlr_region_expand(damage.pixman(), damage.pixman(), BLURRADIUS); // expand for proper blurring

            pMonitor->lastFrameDamage = damage;

            wlr_region_expand(damage.pixman(), damage.pixman(), BLURRADIUS); // expand for proper blurring 2
        } else {
            pMonitor->lastFrameDamage = damage;
        }
    }

    if (pMonitor->forceFullFrames > 0) {
        pMonitor->forceFullFrames -= 1;
        if (pMonitor->forceFullFrames > 10)
            pMonitor->forceFullFrames = 0;
    }

    // TODO: this is getting called with extents being 0,0,0,0 should it be?
    // potentially can save on resources.

    TRACY_GPU_ZONE("Render");

    g_pHyprOpenGL->begin(pMonitor, &damage);

    EMIT_HOOK_EVENT("render", RENDER_BEGIN);

    bool renderCursor = true;

    if (!pMonitor->solitaryClient) {
        if (pMonitor->isMirror()) {
            g_pHyprOpenGL->blend(false);
            g_pHyprOpenGL->renderMirrored();
            g_pHyprOpenGL->blend(true);
            EMIT_HOOK_EVENT("render", RENDER_POST_MIRROR);
            renderCursor = false;
        } else {
            g_pHyprOpenGL->blend(false);
            if (!canSkipBackBufferClear(pMonitor)) {
                if (*PRENDERTEX /* inverted cfg flag */)
                    g_pHyprOpenGL->clear(CColor(*PBACKGROUNDCOLOR));
                else
                    g_pHyprOpenGL->clearWithTex(); // will apply the hypr "wallpaper"
            }
            g_pHyprOpenGL->blend(true);

            wlr_box renderBox = {0, 0, (int)pMonitor->vecPixelSize.x, (int)pMonitor->vecPixelSize.y};
            renderWorkspace(pMonitor, g_pCompositor->getWorkspaceByID(pMonitor->activeWorkspace), &now, renderBox);

            renderLockscreen(pMonitor, &now);

            if (pMonitor == g_pCompositor->m_pLastMonitor) {
                g_pHyprNotificationOverlay->draw(pMonitor);
                g_pHyprError->draw();
            }

            // for drawing the debug overlay
            if (pMonitor == g_pCompositor->m_vMonitors.front().get() && *PDEBUGOVERLAY == 1) {
                startRenderOverlay = std::chrono::high_resolution_clock::now();
                g_pDebugOverlay->draw();
                endRenderOverlay = std::chrono::high_resolution_clock::now();
            }

            if (*PDAMAGEBLINK && damageBlinkCleanup == 0) {
                wlr_box monrect = {0, 0, pMonitor->vecTransformedSize.x, pMonitor->vecTransformedSize.y};
                g_pHyprOpenGL->renderRect(&monrect, CColor(1.0, 0.0, 1.0, 100.0 / 255.0), 0);
                damageBlinkCleanup = 1;
            } else if (*PDAMAGEBLINK) {
                damageBlinkCleanup++;
                if (damageBlinkCleanup > 3)
                    damageBlinkCleanup = 0;
            }
        }
    } else {
        g_pHyprRenderer->renderWindow(pMonitor->solitaryClient, pMonitor, &now, false, RENDER_PASS_MAIN /* solitary = no popups */);
    }

    renderCursor = renderCursor && shouldRenderCursor();

    if (renderCursor && wlr_renderer_begin(g_pCompositor->m_sWLRRenderer, pMonitor->vecPixelSize.x, pMonitor->vecPixelSize.y)) {
        TRACY_GPU_ZONE("RenderCursor");

        bool lockSoftware = pMonitor == g_pCompositor->getMonitorFromCursor() && *PZOOMFACTOR != 1.f;

        if (lockSoftware) {
            wlr_output_lock_software_cursors(pMonitor->output, true);
            wlr_output_render_software_cursors(pMonitor->output, NULL);
            wlr_output_lock_software_cursors(pMonitor->output, false);
        } else
            wlr_output_render_software_cursors(pMonitor->output, NULL);

        wlr_renderer_end(g_pCompositor->m_sWLRRenderer);
    }

    if (pMonitor == g_pCompositor->getMonitorFromCursor())
        g_pHyprOpenGL->m_RenderData.mouseZoomFactor = std::clamp(*PZOOMFACTOR, 1.f, INFINITY);
    else
        g_pHyprOpenGL->m_RenderData.mouseZoomFactor = 1.f;

    if (zoomInFactorFirstLaunch > 1.f) {
        g_pHyprOpenGL->m_RenderData.mouseZoomFactor    = zoomInFactorFirstLaunch;
        g_pHyprOpenGL->m_RenderData.mouseZoomUseMouse  = false;
        g_pHyprOpenGL->m_RenderData.useNearestNeighbor = false;
    }

    EMIT_HOOK_EVENT("render", RENDER_LAST_MOMENT);

    g_pHyprOpenGL->end();

    TRACY_GPU_COLLECT;

    // calc frame damage
    CRegion    frameDamage{};

    const auto TRANSFORM = wlr_output_transform_invert(pMonitor->output->transform);
    wlr_region_transform(frameDamage.pixman(), pMonitor->lastFrameDamage.pixman(), TRANSFORM, (int)pMonitor->vecTransformedSize.x, (int)pMonitor->vecTransformedSize.y);

    if (*PDAMAGETRACKINGMODE == DAMAGE_TRACKING_NONE || *PDAMAGETRACKINGMODE == DAMAGE_TRACKING_MONITOR)
        frameDamage.add(0, 0, (int)pMonitor->vecTransformedSize.x, (int)pMonitor->vecTransformedSize.y);

    if (*PDAMAGEBLINK)
        frameDamage.add(damage);

    if (!pMonitor->mirrors.empty())
        g_pHyprRenderer->damageMirrorsWith(pMonitor, frameDamage);

    pMonitor->renderingActive = false;

    EMIT_HOOK_EVENT("render", RENDER_POST);

    pMonitor->output->pending.tearing_page_flip = shouldTear;

    if (!wlr_output_commit(pMonitor->output)) {

        if (UNLOCK_SC)
            wlr_output_lock_software_cursors(pMonitor->output, false);

        return;
    }

    if (shouldTear)
        pMonitor->tearingState.busy = true;

    wlr_damage_ring_rotate(&pMonitor->damage);

    if (UNLOCK_SC)
        wlr_output_lock_software_cursors(pMonitor->output, false);

    if (*PDAMAGEBLINK || *PVFR == 0 || pMonitor->pendingFrame)
        g_pCompositor->scheduleFrameForMonitor(pMonitor);

    pMonitor->pendingFrame = false;

    const float µs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - startRender).count() / 1000.f;
    g_pDebugOverlay->renderData(pMonitor, µs);

    if (*PDEBUGOVERLAY == 1) {
        if (pMonitor == g_pCompositor->m_vMonitors.front().get()) {
            const float µsNoOverlay = µs - std::chrono::duration_cast<std::chrono::nanoseconds>(endRenderOverlay - startRenderOverlay).count() / 1000.f;
            g_pDebugOverlay->renderDataNoOverlay(pMonitor, µsNoOverlay);
        } else {
            g_pDebugOverlay->renderDataNoOverlay(pMonitor, µs);
        }
    }
}

void CHyprRenderer::renderWorkspace(CMonitor* pMonitor, CWorkspace* pWorkspace, timespec* now, const wlr_box& geometry) {
    Vector2D translate = {geometry.x, geometry.y};
    float    scale     = (float)geometry.width / pMonitor->vecPixelSize.x;

    TRACY_GPU_ZONE("RenderWorkspace");

    if (!DELTALESSTHAN((double)geometry.width / (double)geometry.height, pMonitor->vecPixelSize.x / pMonitor->vecPixelSize.y, 0.01)) {
        Debug::log(ERR, "Ignoring geometry in renderWorkspace: aspect ratio mismatch");
        scale     = 1.f;
        translate = Vector2D{};
    }

    g_pHyprOpenGL->m_RenderData.pWorkspace = pWorkspace;
    renderAllClientsForWorkspace(pMonitor, pWorkspace, now, translate, scale);
    g_pHyprOpenGL->m_RenderData.pWorkspace = nullptr;
}

void CHyprRenderer::setWindowScanoutMode(CWindow* pWindow) {
    if (!g_pCompositor->m_sWLRLinuxDMABuf || g_pSessionLockManager->isSessionLocked())
        return;

    if (!pWindow->m_bIsFullscreen) {
        wlr_linux_dmabuf_v1_set_surface_feedback(g_pCompositor->m_sWLRLinuxDMABuf, pWindow->m_pWLSurface.wlr(), nullptr);
        Debug::log(LOG, "Scanout mode OFF set for {}", pWindow);
        return;
    }

    const auto                                      PMONITOR = g_pCompositor->getMonitorFromID(pWindow->m_iMonitorID);

    const wlr_linux_dmabuf_feedback_v1_init_options INIT_OPTIONS = {
        .main_renderer          = g_pCompositor->m_sWLRRenderer,
        .scanout_primary_output = PMONITOR->output,
    };

    wlr_linux_dmabuf_feedback_v1 feedback = {0};

    if (!wlr_linux_dmabuf_feedback_v1_init_with_options(&feedback, &INIT_OPTIONS))
        return;

    wlr_linux_dmabuf_v1_set_surface_feedback(g_pCompositor->m_sWLRLinuxDMABuf, pWindow->m_pWLSurface.wlr(), &feedback);
    wlr_linux_dmabuf_feedback_v1_finish(&feedback);

    Debug::log(LOG, "Scanout mode ON set for {}", pWindow);
}

void CHyprRenderer::outputMgrApplyTest(wlr_output_configuration_v1* config, bool test) {
    wlr_output_configuration_head_v1* head;
    bool                              ok = true;

    wl_list_for_each(head, &config->heads, link) {

        std::string commandForCfg = "";
        const auto  OUTPUT        = head->state.output;

        commandForCfg += std::string(OUTPUT->name) + ",";

        if (!head->state.enabled) {
            commandForCfg += "disabled";
            if (!test)
                g_pConfigManager->parseKeyword("monitor", commandForCfg, true);
            continue;
        }

        wlr_output_enable(OUTPUT, head->state.enabled);

        if (head->state.mode)
            commandForCfg +=
                std::to_string(head->state.mode->width) + "x" + std::to_string(head->state.mode->height) + "@" + std::to_string(head->state.mode->refresh / 1000.f) + ",";
        else
            commandForCfg += std::to_string(head->state.custom_mode.width) + "x" + std::to_string(head->state.custom_mode.height) + "@" +
                std::to_string(head->state.custom_mode.refresh / 1000.f) + ",";

        commandForCfg += std::to_string(head->state.x) + "x" + std::to_string(head->state.y) + "," + std::to_string(head->state.scale) + ",transform," +
            std::to_string((int)head->state.transform);

        if (!test) {
            g_pConfigManager->parseKeyword("monitor", commandForCfg, true);
            wlr_output_state_set_adaptive_sync_enabled(&OUTPUT->pending, head->state.adaptive_sync_enabled);
        }

        ok = wlr_output_test(OUTPUT);

        if (!ok)
            break;
    }

    if (!test)
        g_pConfigManager->m_bWantsMonitorReload = true; // for monitor keywords

    if (ok)
        wlr_output_configuration_v1_send_succeeded(config);
    else
        wlr_output_configuration_v1_send_failed(config);
    wlr_output_configuration_v1_destroy(config);

    Debug::log(LOG, "OutputMgr Applied/Tested.");
}

// taken from Sway.
// this is just too much of a spaghetti for me to understand
void apply_exclusive(struct wlr_box* usable_area, uint32_t anchor, int32_t exclusive, int32_t margin_top, int32_t margin_right, int32_t margin_bottom, int32_t margin_left) {
    if (exclusive <= 0) {
        return;
    }
    struct {
        uint32_t singular_anchor;
        uint32_t anchor_triplet;
        int*     positive_axis;
        int*     negative_axis;
        int      margin;
    } edges[] = {
        // Top
        {
            .singular_anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
            .anchor_triplet  = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT | ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
            .positive_axis   = &usable_area->y,
            .negative_axis   = &usable_area->height,
            .margin          = margin_top,
        },
        // Bottom
        {
            .singular_anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
            .anchor_triplet  = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
            .positive_axis   = NULL,
            .negative_axis   = &usable_area->height,
            .margin          = margin_bottom,
        },
        // Left
        {
            .singular_anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT,
            .anchor_triplet  = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
            .positive_axis   = &usable_area->x,
            .negative_axis   = &usable_area->width,
            .margin          = margin_left,
        },
        // Right
        {
            .singular_anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
            .anchor_triplet  = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT | ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
            .positive_axis   = NULL,
            .negative_axis   = &usable_area->width,
            .margin          = margin_right,
        },
    };
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); ++i) {
        if ((anchor == edges[i].singular_anchor || anchor == edges[i].anchor_triplet) && exclusive + edges[i].margin > 0) {
            if (edges[i].positive_axis) {
                *edges[i].positive_axis += exclusive + edges[i].margin;
            }
            if (edges[i].negative_axis) {
                *edges[i].negative_axis -= exclusive + edges[i].margin;
            }
            break;
        }
    }
}

void CHyprRenderer::arrangeLayerArray(CMonitor* pMonitor, const std::vector<std::unique_ptr<SLayerSurface>>& layerSurfaces, bool exclusiveZone, wlr_box* usableArea) {
    wlr_box full_area = {pMonitor->vecPosition.x, pMonitor->vecPosition.y, pMonitor->vecSize.x, pMonitor->vecSize.y};

    for (auto& ls : layerSurfaces) {
        if (ls->fadingOut || ls->readyToDelete || !ls->layerSurface || ls->noProcess)
            continue;

        const auto PLAYER = ls->layerSurface;
        const auto PSTATE = &PLAYER->current;
        if (exclusiveZone != (PSTATE->exclusive_zone > 0))
            continue;

        wlr_box bounds;
        if (PSTATE->exclusive_zone == -1)
            bounds = full_area;
        else
            bounds = *usableArea;

        const Vector2D OLDSIZE = {ls->geometry.width, ls->geometry.height};

        wlr_box        box = {.width = PSTATE->desired_width, .height = PSTATE->desired_height};
        // Horizontal axis
        const uint32_t both_horiz = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        if (box.width == 0) {
            box.x = bounds.x;
        } else if ((PSTATE->anchor & both_horiz) == both_horiz) {
            box.x = bounds.x + ((bounds.width / 2) - (box.width / 2));
        } else if ((PSTATE->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT)) {
            box.x = bounds.x;
        } else if ((PSTATE->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) {
            box.x = bounds.x + (bounds.width - box.width);
        } else {
            box.x = bounds.x + ((bounds.width / 2) - (box.width / 2));
        }
        // Vertical axis
        const uint32_t both_vert = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
        if (box.height == 0) {
            box.y = bounds.y;
        } else if ((PSTATE->anchor & both_vert) == both_vert) {
            box.y = bounds.y + ((bounds.height / 2) - (box.height / 2));
        } else if ((PSTATE->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)) {
            box.y = bounds.y;
        } else if ((PSTATE->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)) {
            box.y = bounds.y + (bounds.height - box.height);
        } else {
            box.y = bounds.y + ((bounds.height / 2) - (box.height / 2));
        }
        // Margin
        if (box.width == 0) {
            box.x += PSTATE->margin.left;
            box.width = bounds.width - (PSTATE->margin.left + PSTATE->margin.right);
        } else if ((PSTATE->anchor & both_horiz) == both_horiz) {
            // don't apply margins
        } else if ((PSTATE->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT)) {
            box.x += PSTATE->margin.left;
        } else if ((PSTATE->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) {
            box.x -= PSTATE->margin.right;
        }
        if (box.height == 0) {
            box.y += PSTATE->margin.top;
            box.height = bounds.height - (PSTATE->margin.top + PSTATE->margin.bottom);
        } else if ((PSTATE->anchor & both_vert) == both_vert) {
            // don't apply margins
        } else if ((PSTATE->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)) {
            box.y += PSTATE->margin.top;
        } else if ((PSTATE->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)) {
            box.y -= PSTATE->margin.bottom;
        }
        if (box.width <= 0 || box.height <= 0) {
            Debug::log(ERR, "LayerSurface {:x} has a negative/zero w/h???", (uintptr_t)ls.get());
            continue;
        }
        // Apply
        ls->geometry = box;

        apply_exclusive(usableArea, PSTATE->anchor, PSTATE->exclusive_zone, PSTATE->margin.top, PSTATE->margin.right, PSTATE->margin.bottom, PSTATE->margin.left);

        if (Vector2D{box.width, box.height} != OLDSIZE)
            wlr_layer_surface_v1_configure(ls->layerSurface, box.width, box.height);

        Debug::log(LOG, "LayerSurface {:x} arranged: x: {} y: {} w: {} h: {} with margins: t: {} l: {} r: {} b: {}", (uintptr_t)&ls, box.x, box.y, box.width, box.height,
                   PSTATE->margin.top, PSTATE->margin.left, PSTATE->margin.right, PSTATE->margin.bottom);
    }
}

void CHyprRenderer::arrangeLayersForMonitor(const int& monitor) {
    const auto PMONITOR = g_pCompositor->getMonitorFromID(monitor);

    if (!PMONITOR)
        return;

    // Reset the reserved
    PMONITOR->vecReservedBottomRight = Vector2D();
    PMONITOR->vecReservedTopLeft     = Vector2D();

    wlr_box usableArea = {PMONITOR->vecPosition.x, PMONITOR->vecPosition.y, PMONITOR->vecSize.x, PMONITOR->vecSize.y};

    for (auto& la : PMONITOR->m_aLayerSurfaceLayers)
        arrangeLayerArray(PMONITOR, la, true, &usableArea);

    for (auto& la : PMONITOR->m_aLayerSurfaceLayers)
        arrangeLayerArray(PMONITOR, la, false, &usableArea);

    PMONITOR->vecReservedTopLeft     = Vector2D(usableArea.x, usableArea.y) - PMONITOR->vecPosition;
    PMONITOR->vecReservedBottomRight = PMONITOR->vecSize - Vector2D(usableArea.width, usableArea.height) - PMONITOR->vecReservedTopLeft;

    auto ADDITIONALRESERVED = g_pConfigManager->m_mAdditionalReservedAreas.find(PMONITOR->szName);
    if (ADDITIONALRESERVED == g_pConfigManager->m_mAdditionalReservedAreas.end()) {
        ADDITIONALRESERVED = g_pConfigManager->m_mAdditionalReservedAreas.find(""); // glob wildcard
    }

    if (ADDITIONALRESERVED != g_pConfigManager->m_mAdditionalReservedAreas.end()) {
        PMONITOR->vecReservedTopLeft     = PMONITOR->vecReservedTopLeft + Vector2D(ADDITIONALRESERVED->second.left, ADDITIONALRESERVED->second.top);
        PMONITOR->vecReservedBottomRight = PMONITOR->vecReservedBottomRight + Vector2D(ADDITIONALRESERVED->second.right, ADDITIONALRESERVED->second.bottom);
    }

    // damage the monitor if can
    damageMonitor(PMONITOR);

    g_pLayoutManager->getCurrentLayout()->recalculateMonitor(monitor);

    Debug::log(LOG, "Monitor {} layers arranged: reserved: {:5j} {:5j}", PMONITOR->szName, PMONITOR->vecReservedTopLeft, PMONITOR->vecReservedBottomRight);
}

void CHyprRenderer::damageSurface(wlr_surface* pSurface, double x, double y, double scale) {
    if (!pSurface)
        return; // wut?

    if (g_pCompositor->m_bUnsafeState)
        return;

    CRegion damageBox;
    wlr_surface_get_effective_damage(pSurface, damageBox.pixman());
    if (scale != 1.0)
        wlr_region_scale(damageBox.pixman(), damageBox.pixman(), scale);

    // schedule frame events
    if (!wl_list_empty(&pSurface->current.frame_callback_list)) {
        g_pCompositor->scheduleFrameForMonitor(g_pCompositor->getMonitorFromVector(Vector2D(x, y)));
    }

    if (damageBox.empty())
        return;

    CRegion damageBoxForEach;

    for (auto& m : g_pCompositor->m_vMonitors) {
        if (!m->output)
            continue;

        double lx = 0, ly = 0;
        wlr_output_layout_output_coords(g_pCompositor->m_sWLROutputLayout, m->output, &lx, &ly);

        damageBoxForEach = damageBox;
        damageBoxForEach.translate({x - m->vecPosition.x, y - m->vecPosition.y});
        wlr_region_scale(damageBoxForEach.pixman(), damageBoxForEach.pixman(), m->scale);
        damageBoxForEach.translate({lx + m->vecPosition.x, ly + m->vecPosition.y});

        m->addDamage(&damageBoxForEach);
    }

    static auto* const PLOGDAMAGE = &g_pConfigManager->getConfigValuePtr("debug:log_damage")->intValue;

    if (*PLOGDAMAGE)
        Debug::log(LOG, "Damage: Surface (extents): xy: {}, {} wh: {}, {}", damageBox.pixman()->extents.x1, damageBox.pixman()->extents.y1,
                   damageBox.pixman()->extents.x2 - damageBox.pixman()->extents.x1, damageBox.pixman()->extents.y2 - damageBox.pixman()->extents.y1);
}

void CHyprRenderer::damageWindow(CWindow* pWindow) {
    if (g_pCompositor->m_bUnsafeState)
        return;

    wlr_box damageBox = pWindow->getFullWindowBoundingBox();
    for (auto& m : g_pCompositor->m_vMonitors) {
        wlr_box fixedDamageBox = {damageBox.x - m->vecPosition.x, damageBox.y - m->vecPosition.y, damageBox.width, damageBox.height};
        scaleBox(&fixedDamageBox, m->scale);
        m->addDamage(&fixedDamageBox);
    }

    for (auto& wd : pWindow->m_dWindowDecorations)
        wd->damageEntire();

    static auto* const PLOGDAMAGE = &g_pConfigManager->getConfigValuePtr("debug:log_damage")->intValue;

    if (*PLOGDAMAGE)
        Debug::log(LOG, "Damage: Window ({}): xy: {}, {} wh: {}, {}", pWindow->m_szTitle, damageBox.x, damageBox.y, damageBox.width, damageBox.height);
}

void CHyprRenderer::damageMonitor(CMonitor* pMonitor) {
    if (g_pCompositor->m_bUnsafeState || pMonitor->isMirror())
        return;

    wlr_box damageBox = {0, 0, INT16_MAX, INT16_MAX};
    pMonitor->addDamage(&damageBox);

    static auto* const PLOGDAMAGE = &g_pConfigManager->getConfigValuePtr("debug:log_damage")->intValue;

    if (*PLOGDAMAGE)
        Debug::log(LOG, "Damage: Monitor {}", pMonitor->szName);
}

void CHyprRenderer::damageBox(wlr_box* pBox) {
    if (g_pCompositor->m_bUnsafeState)
        return;

    for (auto& m : g_pCompositor->m_vMonitors) {
        if (m->isMirror())
            continue; // don't damage mirrors traditionally

        wlr_box damageBox = {pBox->x - m->vecPosition.x, pBox->y - m->vecPosition.y, pBox->width, pBox->height};
        scaleBox(&damageBox, m->scale);
        m->addDamage(&damageBox);
    }

    static auto* const PLOGDAMAGE = &g_pConfigManager->getConfigValuePtr("debug:log_damage")->intValue;

    if (*PLOGDAMAGE)
        Debug::log(LOG, "Damage: Box: xy: {}, {} wh: {}, {}", pBox->x, pBox->y, pBox->width, pBox->height);
}

void CHyprRenderer::damageBox(const int& x, const int& y, const int& w, const int& h) {
    wlr_box box = {x, y, w, h};
    damageBox(&box);
}

void CHyprRenderer::damageRegion(const CRegion& rg) {
    for (auto& RECT : rg.getRects()) {
        damageBox(RECT.x1, RECT.y1, RECT.x2 - RECT.x1, RECT.y2 - RECT.y1);
    }
}

void CHyprRenderer::damageMirrorsWith(CMonitor* pMonitor, const CRegion& pRegion) {
    for (auto& mirror : pMonitor->mirrors) {
        Vector2D scale = {mirror->vecSize.x / pMonitor->vecSize.x, mirror->vecSize.y / pMonitor->vecSize.y};

        CRegion  rg{pRegion};
        wlr_region_scale_xy(rg.pixman(), rg.pixman(), scale.x, scale.y);
        pMonitor->addDamage(&rg);

        g_pCompositor->scheduleFrameForMonitor(mirror);
    }
}

void CHyprRenderer::renderDragIcon(CMonitor* pMonitor, timespec* time) {
    if (!(g_pInputManager->m_sDrag.dragIcon && g_pInputManager->m_sDrag.iconMapped && g_pInputManager->m_sDrag.dragIcon->surface))
        return;

    SRenderData renderdata = {pMonitor, time, g_pInputManager->m_sDrag.pos.x, g_pInputManager->m_sDrag.pos.y};
    renderdata.surface     = g_pInputManager->m_sDrag.dragIcon->surface;
    renderdata.w           = g_pInputManager->m_sDrag.dragIcon->surface->current.width;
    renderdata.h           = g_pInputManager->m_sDrag.dragIcon->surface->current.height;

    wlr_surface_for_each_surface(g_pInputManager->m_sDrag.dragIcon->surface, renderSurface, &renderdata);

    wlr_box box = {g_pInputManager->m_sDrag.pos.x - 2, g_pInputManager->m_sDrag.pos.y - 2, g_pInputManager->m_sDrag.dragIcon->surface->current.width + 4,
                   g_pInputManager->m_sDrag.dragIcon->surface->current.height + 4};
    g_pHyprRenderer->damageBox(&box);
}

DAMAGETRACKINGMODES CHyprRenderer::damageTrackingModeFromStr(const std::string& mode) {
    if (mode == "full")
        return DAMAGE_TRACKING_FULL;
    if (mode == "monitor")
        return DAMAGE_TRACKING_MONITOR;
    if (mode == "none")
        return DAMAGE_TRACKING_NONE;

    return DAMAGE_TRACKING_INVALID;
}

bool CHyprRenderer::applyMonitorRule(CMonitor* pMonitor, SMonitorRule* pMonitorRule, bool force) {

    Debug::log(LOG, "Applying monitor rule for {}", pMonitor->szName);

    pMonitor->activeMonitorRule = *pMonitorRule;

    // if it's disabled, disable and ignore
    if (pMonitorRule->disabled) {

        if (pMonitor->m_bEnabled)
            pMonitor->onDisconnect();

        return true;
    }

    // don't touch VR headsets
    if (pMonitor->output->non_desktop)
        return true;

    if (!pMonitor->m_bEnabled) {
        pMonitor->onConnect(true); // enable it.
        Debug::log(LOG, "Monitor {} is disabled but is requested to be enabled", pMonitor->szName);
        force = true;
    }

    // Check if the rule isn't already applied
    // TODO: clean this up lol
    if (!force && DELTALESSTHAN(pMonitor->vecPixelSize.x, pMonitorRule->resolution.x, 1) && DELTALESSTHAN(pMonitor->vecPixelSize.y, pMonitorRule->resolution.y, 1) &&
        DELTALESSTHAN(pMonitor->refreshRate, pMonitorRule->refreshRate, 1) && pMonitor->scale == pMonitorRule->scale &&
        ((DELTALESSTHAN(pMonitor->vecPosition.x, pMonitorRule->offset.x, 1) && DELTALESSTHAN(pMonitor->vecPosition.y, pMonitorRule->offset.y, 1)) ||
         pMonitorRule->offset == Vector2D(-INT32_MAX, -INT32_MAX)) &&
        pMonitor->transform == pMonitorRule->transform && pMonitorRule->enable10bit == pMonitor->enabled10bit &&
        !memcmp(&pMonitor->customDrmMode, &pMonitorRule->drmMode, sizeof(pMonitor->customDrmMode))) {

        Debug::log(LOG, "Not applying a new rule to {} because it's already applied!", pMonitor->szName);
        return true;
    }

    // Needed in case we are switching from a custom modeline to a standard mode
    pMonitor->customDrmMode = {};

    if (pMonitorRule->scale > 0.1) {
        wlr_output_set_scale(pMonitor->output, pMonitorRule->scale);
        pMonitor->scale = pMonitorRule->scale;
    } else {
        const auto DEFAULTSCALE = pMonitor->getDefaultScale();
        wlr_output_set_scale(pMonitor->output, DEFAULTSCALE);
        pMonitor->scale = DEFAULTSCALE;
    }

    wlr_output_set_transform(pMonitor->output, pMonitorRule->transform);
    pMonitor->transform = pMonitorRule->transform;

    const auto WLRREFRESHRATE = (wlr_backend_is_wl(pMonitor->output->backend) || wlr_backend_is_x11(pMonitor->output->backend)) ? 0 : pMonitorRule->refreshRate * 1000;

    // loop over modes and choose an appropriate one.
    if (pMonitorRule->resolution != Vector2D() && pMonitorRule->resolution != Vector2D(-1, -1) && pMonitorRule->resolution != Vector2D(-1, -2)) {
        if (!wl_list_empty(&pMonitor->output->modes) && pMonitorRule->drmMode.type != DRM_MODE_TYPE_USERDEF) {
            wlr_output_mode* mode;
            bool             found = false;

            wl_list_for_each(mode, &pMonitor->output->modes, link) {
                // if delta of refresh rate, w and h chosen and mode is < 1 we accept it
                if (DELTALESSTHAN(mode->width, pMonitorRule->resolution.x, 1) && DELTALESSTHAN(mode->height, pMonitorRule->resolution.y, 1) &&
                    DELTALESSTHAN(mode->refresh / 1000.f, pMonitorRule->refreshRate, 1)) {
                    wlr_output_set_mode(pMonitor->output, mode);

                    if (!wlr_output_test(pMonitor->output)) {
                        Debug::log(LOG, "Monitor {}: REJECTED available mode: {}x{}@{:2f}!", pMonitor->output->name, mode->width, mode->height, mode->refresh / 1000.f);
                        continue;
                    }

                    Debug::log(LOG, "Monitor {}: requested {:X0}@{:2f}, found available mode: {}x{}@{}mHz, applying.", pMonitor->output->name, pMonitorRule->resolution,
                               (float)pMonitorRule->refreshRate, mode->width, mode->height, mode->refresh);

                    found = true;

                    pMonitor->refreshRate = mode->refresh / 1000.f;
                    pMonitor->vecSize     = Vector2D(mode->width, mode->height);

                    break;
                }
            }

            if (!found) {
                wlr_output_set_custom_mode(pMonitor->output, (int)pMonitorRule->resolution.x, (int)pMonitorRule->resolution.y, WLRREFRESHRATE);
                pMonitor->vecSize     = pMonitorRule->resolution;
                pMonitor->refreshRate = pMonitorRule->refreshRate;

                if (!wlr_output_test(pMonitor->output)) {
                    Debug::log(ERR, "Custom resolution FAILED, falling back to preferred");

                    const auto PREFERREDMODE = wlr_output_preferred_mode(pMonitor->output);

                    if (!PREFERREDMODE) {
                        Debug::log(ERR, "Monitor {} has NO PREFERRED MODE, and an INVALID one was requested: {:X0}@{:2f}", pMonitor->ID, pMonitorRule->resolution,
                                   (float)pMonitorRule->refreshRate);
                        return true;
                    }

                    // Preferred is valid
                    wlr_output_set_mode(pMonitor->output, PREFERREDMODE);

                    Debug::log(ERR, "Monitor {} got an invalid requested mode: {:X0}@{:2f}, using the preferred one instead: {}x{}@{:2f}", pMonitor->output->name,
                               pMonitorRule->resolution, (float)pMonitorRule->refreshRate, PREFERREDMODE->width, PREFERREDMODE->height, PREFERREDMODE->refresh / 1000.f);

                    pMonitor->refreshRate = PREFERREDMODE->refresh / 1000.f;
                    pMonitor->vecSize     = Vector2D(PREFERREDMODE->width, PREFERREDMODE->height);
                } else {
                    Debug::log(LOG, "Set a custom mode {:X0}@{:2f} (mode not found in monitor modes)", pMonitorRule->resolution, (float)pMonitorRule->refreshRate);
                }
            }
        } else {
            // custom resolution
            bool fail = false;

            if (pMonitorRule->drmMode.type == DRM_MODE_TYPE_USERDEF) {
                if (!wlr_output_is_drm(pMonitor->output)) {
                    Debug::log(ERR, "Tried to set custom modeline on non-DRM output");
                    fail = true;
                } else {
                    auto* mode = wlr_drm_connector_add_mode(pMonitor->output, &pMonitorRule->drmMode);
                    if (mode) {
                        wlr_output_set_mode(pMonitor->output, mode);
                        pMonitor->customDrmMode = pMonitorRule->drmMode;
                    } else {
                        Debug::log(ERR, "wlr_drm_connector_add_mode failed");
                        fail = true;
                    }
                }
            } else {
                wlr_output_set_custom_mode(pMonitor->output, (int)pMonitorRule->resolution.x, (int)pMonitorRule->resolution.y, WLRREFRESHRATE);
            }

            pMonitor->vecSize     = pMonitorRule->resolution;
            pMonitor->refreshRate = pMonitorRule->refreshRate;

            if (fail || !wlr_output_test(pMonitor->output)) {
                Debug::log(ERR, "Custom resolution FAILED, falling back to preferred");

                const auto PREFERREDMODE = wlr_output_preferred_mode(pMonitor->output);

                if (!PREFERREDMODE) {
                    Debug::log(ERR, "Monitor {} has NO PREFERRED MODE, and an INVALID one was requested: {:X0}@{:2f}", pMonitor->output->name, pMonitorRule->resolution,
                               (float)pMonitorRule->refreshRate);
                    return true;
                }

                // Preferred is valid
                wlr_output_set_mode(pMonitor->output, PREFERREDMODE);

                Debug::log(ERR, "Monitor {} got an invalid requested mode: {:X0}@{:2f}, using the preferred one instead: {}x{}@{:2f}", pMonitor->output->name,
                           pMonitorRule->resolution, (float)pMonitorRule->refreshRate, PREFERREDMODE->width, PREFERREDMODE->height, PREFERREDMODE->refresh / 1000.f);

                pMonitor->refreshRate   = PREFERREDMODE->refresh / 1000.f;
                pMonitor->vecSize       = Vector2D(PREFERREDMODE->width, PREFERREDMODE->height);
                pMonitor->customDrmMode = {};
            } else {
                Debug::log(LOG, "Set a custom mode {:X0}@{:2f} (mode not found in monitor modes)", pMonitorRule->resolution, (float)pMonitorRule->refreshRate);
            }
        }
    } else if (pMonitorRule->resolution != Vector2D()) {
        if (!wl_list_empty(&pMonitor->output->modes)) {
            wlr_output_mode* mode;
            float            currentWidth   = 0;
            float            currentHeight  = 0;
            float            currentRefresh = 0;
            bool             success        = false;

            //(-1,-1) indicates a preference to refreshrate over resolution, (-1,-2) preference to resolution
            if (pMonitorRule->resolution == Vector2D(-1, -1)) {
                wl_list_for_each(mode, &pMonitor->output->modes, link) {
                    if ((mode->width >= currentWidth && mode->height >= currentHeight && mode->refresh >= (currentRefresh - 1000.f)) || mode->refresh > (currentRefresh + 3000.f)) {
                        wlr_output_set_mode(pMonitor->output, mode);
                        if (wlr_output_test(pMonitor->output)) {
                            currentWidth   = mode->width;
                            currentHeight  = mode->height;
                            currentRefresh = mode->refresh;
                            success        = true;
                        }
                    }
                }
            } else {
                wl_list_for_each(mode, &pMonitor->output->modes, link) {
                    if ((mode->width >= currentWidth && mode->height >= currentHeight && mode->refresh >= (currentRefresh - 1000.f)) ||
                        (mode->width > currentWidth && mode->height > currentHeight)) {
                        wlr_output_set_mode(pMonitor->output, mode);
                        if (wlr_output_test(pMonitor->output)) {
                            currentWidth   = mode->width;
                            currentHeight  = mode->height;
                            currentRefresh = mode->refresh;
                            success        = true;
                        }
                    }
                }
            }

            if (!success) {
                Debug::log(LOG, "Monitor {}: REJECTED mode: {:X0}@{:2f}! Falling back to preferred: {}x{}@{:2f}", pMonitor->output->name, pMonitorRule->resolution,
                           (float)pMonitorRule->refreshRate, mode->width, mode->height, mode->refresh / 1000.f);

                const auto PREFERREDMODE = wlr_output_preferred_mode(pMonitor->output);

                if (!PREFERREDMODE) {
                    Debug::log(ERR, "Monitor {} has NO PREFERRED MODE, and an INVALID one was requested: {:X0}@{:2f}", pMonitor->ID, pMonitorRule->resolution,
                               (float)pMonitorRule->refreshRate);
                    return true;
                }

                // Preferred is valid
                wlr_output_set_mode(pMonitor->output, PREFERREDMODE);

                Debug::log(ERR, "Monitor {} got an invalid requested mode: {:X0}@{:2f}, using the preferred one instead: {}x{}@{:2f}", pMonitor->output->name,
                           pMonitorRule->resolution, (float)pMonitorRule->refreshRate, PREFERREDMODE->width, PREFERREDMODE->height, PREFERREDMODE->refresh / 1000.f);

                pMonitor->refreshRate = PREFERREDMODE->refresh / 1000.f;
                pMonitor->vecSize     = Vector2D(PREFERREDMODE->width, PREFERREDMODE->height);
            } else {

                Debug::log(LOG, "Monitor {}: Applying highest mode {}x{}@{:2f}.", pMonitor->output->name, (int)currentWidth, (int)currentHeight, (int)currentRefresh / 1000.f);

                pMonitor->refreshRate = currentRefresh / 1000.f;
                pMonitor->vecSize     = Vector2D(currentWidth, currentHeight);
            }
        }
    } else {
        const auto PREFERREDMODE = wlr_output_preferred_mode(pMonitor->output);

        if (!PREFERREDMODE) {
            Debug::log(ERR, "Monitor {} has NO PREFERRED MODE", pMonitor->output->name);

            if (!wl_list_empty(&pMonitor->output->modes)) {
                wlr_output_mode* mode;

                wl_list_for_each(mode, &pMonitor->output->modes, link) {
                    wlr_output_set_mode(pMonitor->output, mode);

                    if (!wlr_output_test(pMonitor->output)) {
                        Debug::log(LOG, "Monitor {}: REJECTED available mode: {}x{}@{:2f}!", pMonitor->output->name, mode->width, mode->height, mode->refresh / 1000.f);
                        continue;
                    }

                    Debug::log(LOG, "Monitor {}: requested {:X0}@{:2f}, found available mode: {}x{}@{}mHz, applying.", pMonitor->output->name, pMonitorRule->resolution,
                               (float)pMonitorRule->refreshRate, mode->width, mode->height, mode->refresh);

                    pMonitor->refreshRate = mode->refresh / 1000.f;
                    pMonitor->vecSize     = Vector2D(mode->width, mode->height);

                    break;
                }
            }
        } else {
            // Preferred is valid
            wlr_output_set_mode(pMonitor->output, PREFERREDMODE);

            pMonitor->vecSize     = Vector2D(PREFERREDMODE->width, PREFERREDMODE->height);
            pMonitor->refreshRate = PREFERREDMODE->refresh / 1000.f;

            Debug::log(LOG, "Setting preferred mode for {}", pMonitor->output->name);
        }
    }

    pMonitor->vrrActive = pMonitor->output->pending.adaptive_sync_enabled; // disabled here, will be tested in CConfigManager::ensureVRR()

    pMonitor->vecPixelSize = pMonitor->vecSize;

    // clang-format off
    static const std::array<std::vector<std::pair<std::string, uint32_t>>, 2> formats{
        std::vector<std::pair<std::string, uint32_t>>{ /* 10-bit */
            {"DRM_FORMAT_XRGB2101010", DRM_FORMAT_XRGB2101010}, {"DRM_FORMAT_XBGR2101010", DRM_FORMAT_XBGR2101010}, {"DRM_FORMAT_XRGB8888", DRM_FORMAT_XRGB8888}, {"DRM_FORMAT_XBGR8888", DRM_FORMAT_XBGR8888}, {"DRM_FORMAT_INVALID", DRM_FORMAT_INVALID}
        },
        std::vector<std::pair<std::string, uint32_t>>{ /* 8-bit */
            {"DRM_FORMAT_XRGB8888", DRM_FORMAT_XRGB8888}, {"DRM_FORMAT_XBGR8888", DRM_FORMAT_XBGR8888}, {"DRM_FORMAT_INVALID", DRM_FORMAT_INVALID}
        }
    };
    // clang-format on

    bool set10bit = false;

    for (auto& fmt : formats[(int)!pMonitorRule->enable10bit]) {
        wlr_output_set_render_format(pMonitor->output, fmt.second);

        if (!wlr_output_test(pMonitor->output)) {
            Debug::log(ERR, "output {} failed basic test on format {}", pMonitor->szName, fmt.first);
        } else {
            Debug::log(LOG, "output {} succeeded basic test on format {}", pMonitor->szName, fmt.first);
            if (pMonitorRule->enable10bit && fmt.first.contains("101010"))
                set10bit = true;
            break;
        }
    }

    pMonitor->enabled10bit = set10bit;

    if (!wlr_output_commit(pMonitor->output)) {
        Debug::log(ERR, "Couldn't commit output named {}", pMonitor->output->name);
    }

    int x, y;
    wlr_output_transformed_resolution(pMonitor->output, &x, &y);
    pMonitor->vecSize            = (Vector2D(x, y) / pMonitor->scale).floor();
    pMonitor->vecTransformedSize = Vector2D(x, y);

    if (pMonitor->createdByUser) {
        wlr_box transformedBox = {0, 0, (int)pMonitor->vecTransformedSize.x, (int)pMonitor->vecTransformedSize.y};
        wlr_box_transform(&transformedBox, &transformedBox, wlr_output_transform_invert(pMonitor->output->transform), (int)pMonitor->vecTransformedSize.x,
                          (int)pMonitor->vecTransformedSize.y);

        pMonitor->vecPixelSize = Vector2D(transformedBox.width, transformedBox.height);
    }

    // update renderer (here because it will call rollback, so we cannot do this before committing)
    g_pHyprOpenGL->destroyMonitorResources(pMonitor);

    // updato wlroots
    g_pCompositor->arrangeMonitors();

    wlr_damage_ring_set_bounds(&pMonitor->damage, pMonitor->vecTransformedSize.x, pMonitor->vecTransformedSize.y);

    // updato us
    arrangeLayersForMonitor(pMonitor->ID);

    // frame skip
    pMonitor->framesToSkip = 1;

    // reload to fix mirrors
    g_pConfigManager->m_bWantsMonitorReload = true;

    Debug::log(LOG, "Monitor {} data dump: res {:X}@{:.2f}Hz, scale {:.2f}, transform {}, pos {:X}, 10b {}", pMonitor->szName, pMonitor->vecPixelSize, pMonitor->refreshRate,
               pMonitor->scale, (int)pMonitor->transform, pMonitor->vecPosition, (int)pMonitor->enabled10bit);

    EMIT_HOOK_EVENT("monitorLayoutChanged", nullptr);

    Events::listener_change(nullptr, nullptr);

    return true;
}

void CHyprRenderer::setCursorSurface(wlr_surface* surf, int hotspotX, int hotspotY) {
    m_bCursorHasSurface = surf;

    wlr_cursor_set_surface(g_pCompositor->m_sWLRCursor, surf, hotspotX, hotspotY);
}
void CHyprRenderer::setCursorFromName(const std::string& name) {
    m_bCursorHasSurface = true;

    wlr_cursor_set_xcursor(g_pCompositor->m_sWLRCursor, g_pCompositor->m_sWLRXCursorMgr, name.c_str());
}

void CHyprRenderer::ensureCursorRenderingMode() {
    static auto* const PCURSORTIMEOUT = &g_pConfigManager->getConfigValuePtr("general:cursor_inactive_timeout")->intValue;
    static auto* const PHIDEONTOUCH   = &g_pConfigManager->getConfigValuePtr("misc:hide_cursor_on_touch")->intValue;

    const auto         PASSEDCURSORSECONDS = g_pInputManager->m_tmrLastCursorMovement.getSeconds();

    if (*PCURSORTIMEOUT > 0 || *PHIDEONTOUCH) {
        const bool HIDE = (*PCURSORTIMEOUT > 0 && *PCURSORTIMEOUT < PASSEDCURSORSECONDS) || (g_pInputManager->m_bLastInputTouch && *PHIDEONTOUCH);

        if (HIDE && m_bHasARenderedCursor) {
            m_bHasARenderedCursor = false;

            setCursorSurface(nullptr, 0, 0); // hide

            Debug::log(LOG, "Hiding the cursor (timeout)");

            for (auto& m : g_pCompositor->m_vMonitors)
                g_pHyprRenderer->damageMonitor(m.get()); // TODO: maybe just damage the cursor area?
        } else if (!HIDE && !m_bHasARenderedCursor) {
            m_bHasARenderedCursor = true;

            if (!m_bWindowRequestedCursorHide)
                setCursorFromName("left_ptr");

            Debug::log(LOG, "Showing the cursor (timeout)");

            for (auto& m : g_pCompositor->m_vMonitors)
                g_pHyprRenderer->damageMonitor(m.get()); // TODO: maybe just damage the cursor area?
        }
    } else {
        m_bHasARenderedCursor = true;
    }
}

bool CHyprRenderer::shouldRenderCursor() {
    return m_bHasARenderedCursor && m_bCursorHasSurface;
}

std::tuple<float, float, float> CHyprRenderer::getRenderTimes(CMonitor* pMonitor) {
    const auto POVERLAY = &g_pDebugOverlay->m_mMonitorOverlays[pMonitor];

    float      avgRenderTime = 0;
    float      maxRenderTime = 0;
    float      minRenderTime = 9999;
    for (auto& rt : POVERLAY->m_dLastRenderTimes) {
        if (rt > maxRenderTime)
            maxRenderTime = rt;
        if (rt < minRenderTime)
            minRenderTime = rt;
        avgRenderTime += rt;
    }
    avgRenderTime /= POVERLAY->m_dLastRenderTimes.size() == 0 ? 1 : POVERLAY->m_dLastRenderTimes.size();

    return std::make_tuple<>(avgRenderTime, maxRenderTime, minRenderTime);
}

static int handleCrashLoop(void* data) {

    g_pHyprNotificationOverlay->addNotification("Hyprland will crash in " + std::to_string(10 - (int)(g_pHyprRenderer->m_fCrashingDistort * 2.f)) + "s.", CColor(0), 5000,
                                                ICON_INFO);

    g_pHyprRenderer->m_fCrashingDistort += 0.5f;

    if (g_pHyprRenderer->m_fCrashingDistort >= 5.5f)
        raise(SIGABRT);

    wl_event_source_timer_update(g_pHyprRenderer->m_pCrashingLoop, 1000);

    return 1;
}

void CHyprRenderer::initiateManualCrash() {
    g_pHyprNotificationOverlay->addNotification("Manual crash initiated. Farewell...", CColor(0), 5000, ICON_INFO);

    m_pCrashingLoop = wl_event_loop_add_timer(g_pCompositor->m_sWLEventLoop, handleCrashLoop, nullptr);
    wl_event_source_timer_update(m_pCrashingLoop, 1000);

    m_bCrashingInProgress = true;
    m_fCrashingDistort    = 0.5;

    g_pHyprOpenGL->m_tGlobalTimer.reset();

    g_pConfigManager->setInt("debug:damage_tracking", 0);
}

void CHyprRenderer::setOccludedForBackLayers(CRegion& region, CWorkspace* pWorkspace) {
    CRegion    rg;

    const auto PMONITOR = g_pCompositor->getMonitorFromID(pWorkspace->m_iMonitorID);

    for (auto& w : g_pCompositor->m_vWindows) {
        if (!w->m_bIsMapped || w->isHidden() || w->m_iWorkspaceID != pWorkspace->m_iID)
            continue;

        if (!w->opaque())
            continue;

        const auto     ROUNDING = w->rounding() * PMONITOR->scale;
        const Vector2D POS      = w->m_vRealPosition.vec() + Vector2D{ROUNDING, ROUNDING} - PMONITOR->vecPosition + (w->m_bPinned ? Vector2D{} : pWorkspace->m_vRenderOffset.vec());
        const Vector2D SIZE     = w->m_vRealSize.vec() - Vector2D{ROUNDING * 2, ROUNDING * 2};

        wlr_box        box = {POS.x, POS.y, SIZE.x, SIZE.y};

        scaleBox(&box, PMONITOR->scale);

        rg.add(&box);
    }

    region.subtract(rg);
}

bool CHyprRenderer::canSkipBackBufferClear(CMonitor* pMonitor) {
    for (auto& ls : pMonitor->m_aLayerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND]) {
        if (!ls->layerSurface)
            continue;

        if (ls->alpha.fl() < 1.f)
            continue;

        if (ls->geometry.x != pMonitor->vecPosition.x || ls->geometry.y != pMonitor->vecPosition.y || ls->geometry.width != pMonitor->vecSize.x ||
            ls->geometry.height != pMonitor->vecSize.y)
            continue;

        if (!ls->layerSurface->surface->opaque)
            continue;

        return true;
    }

    return false;
}

void CHyprRenderer::recheckSolitaryForMonitor(CMonitor* pMonitor) {
    pMonitor->solitaryClient = nullptr; // reset it, if we find one it will be set.

    const auto PWORKSPACE = g_pCompositor->getWorkspaceByID(pMonitor->activeWorkspace);

    if (!PWORKSPACE || !PWORKSPACE->m_bHasFullscreenWindow || g_pInputManager->m_sDrag.drag || g_pCompositor->m_sSeat.exclusiveClient || pMonitor->specialWorkspaceID ||
        PWORKSPACE->m_fAlpha.fl() != 1.f || PWORKSPACE->m_vRenderOffset.vec() != Vector2D{})
        return;

    const auto PCANDIDATE = g_pCompositor->getFullscreenWindowOnWorkspace(PWORKSPACE->m_iID);

    if (!PCANDIDATE)
        return; // ????

    if (!PCANDIDATE->opaque())
        return;

    if (PCANDIDATE->m_vRealSize.vec() != pMonitor->vecSize || PCANDIDATE->m_vRealPosition.vec() != pMonitor->vecPosition || PCANDIDATE->m_vRealPosition.isBeingAnimated() ||
        PCANDIDATE->m_vRealSize.isBeingAnimated())
        return;

    if (!pMonitor->m_aLayerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY].empty())
        return;

    for (auto& topls : pMonitor->m_aLayerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP]) {
        if (topls->alpha.fl() != 0.f)
            return;
    }

    for (auto& w : g_pCompositor->m_vWindows) {
        if (w->m_iWorkspaceID == PCANDIDATE->m_iWorkspaceID && w->m_bIsFloating && w->m_bCreatedOverFullscreen && !w->isHidden() && w->m_bIsMapped && w.get() != PCANDIDATE)
            return;
    }

    if (pMonitor->specialWorkspaceID != 0)
        return;

    // check if it did not open any subsurfaces or shit
    int surfaceCount = 0;
    if (PCANDIDATE->m_bIsX11) {
        surfaceCount = 1;
    } else {
        wlr_xdg_surface_for_each_surface(PCANDIDATE->m_uSurface.xdg, countSubsurfacesIter, &surfaceCount);
        wlr_xdg_surface_for_each_popup_surface(PCANDIDATE->m_uSurface.xdg, countSubsurfacesIter, &surfaceCount);
    }

    if (surfaceCount > 1)
        return;

    // found one!
    pMonitor->solitaryClient = PCANDIDATE;
}
