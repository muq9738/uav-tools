/* global Cesium */

const C = Cesium;
const TOP_PITCH = -Math.PI / 2 + C.Math.toRadians(0.6);
const ROTATE_TRIGGER_PX = 5;

/**
 * 与主产品一致的模型中心导航：左键绕表面旋转，右键/中键平移，滚轮朝视口中心缩放。
 * Cesium 默认控制器关闭，避免两套输入叠加造成镜头回弹。
 */
export function installModelCamera(viewer, tileset, { onActivity = () => {} } = {}) {
  const scene = viewer.scene;
  const camera = viewer.camera;
  const canvas = scene.canvas;
  const defaults = scene.screenSpaceCameraController;
  const previousInputs = defaults.enableInputs;
  defaults.enableInputs = false;
  defaults.inertiaSpin = 0;
  defaults.inertiaTranslate = 0;
  defaults.inertiaZoom = 0;

  const handler = new C.ScreenSpaceEventHandler(canvas);
  let center = C.Cartesian3.clone(tileset.boundingSphere.center);
  let rotateStart = null;
  let rotating = false;
  let panStart = null;
  let panning = false;
  let pinch = null;
  let renderFrame = 0;
  let centerTimer = 0;
  let destroyed = false;
  const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)').matches;

  function requestRender() {
    if (destroyed || renderFrame) return;
    renderFrame = requestAnimationFrame(() => {
      renderFrame = 0;
      if (!destroyed && !viewer.isDestroyed()) scene.requestRender();
    });
  }

  function viewportCenter() {
    return new C.Cartesian2(canvas.clientWidth / 2, canvas.clientHeight / 2);
  }

  function surfaceCenter() {
    try {
      const picked = scene.pickPosition(viewportCenter());
      if (picked && C.Cartesian3.distance(picked, tileset.boundingSphere.center) <= tileset.boundingSphere.radius * 4) {
        return C.Cartesian3.clone(picked);
      }
    } catch {
      // 深度纹理暂不可用时沿用最近稳定锚点。
    }
    return C.Cartesian3.clone(center || tileset.boundingSphere.center);
  }

  function refreshCenter() {
    center = surfaceCenter();
  }

  function scheduleCenterRefresh() {
    clearTimeout(centerTimer);
    centerTimer = setTimeout(refreshCenter, 120);
  }

  function beginRotate(position) {
    rotateStart = C.Cartesian2.clone(position);
    rotating = false;
    panStart = null;
    panning = false;
  }

  function rotateTo(position) {
    if (!rotateStart) return;
    let dx = position.x - rotateStart.x;
    let dy = position.y - rotateStart.y;
    if (!rotating) {
      if (Math.hypot(dx, dy) < ROTATE_TRIGGER_PX) return;
      refreshCenter();
      camera.lookAt(center, new C.HeadingPitchRange(
        camera.heading,
        Math.max(camera.pitch, TOP_PITCH),
        C.Cartesian3.distance(camera.position, center),
      ));
      rotating = true;
      onActivity('rotate');
    }
    const ratio = Math.abs(dx) / (Math.abs(dy) + .0001);
    if (ratio <= 2) {
      const nextPitch = C.Math.clamp(camera.pitch - dy * .003, TOP_PITCH, -.25);
      camera.rotateUp(nextPitch - camera.pitch);
    }
    if (ratio >= .5) camera.rotateLeft(dx * .003);
    rotateStart = C.Cartesian2.clone(position);
    requestRender();
  }

  function endRotate() {
    rotateStart = null;
    if (rotating && !camera.transform.equals(C.Matrix4.IDENTITY)) {
      camera.lookAtTransform(C.Matrix4.IDENTITY);
    }
    rotating = false;
    onActivity(null);
    refreshCenter();
    requestRender();
  }

  function worldOnCenterPlane(position) {
    const ray = camera.getPickRay(position);
    if (!ray) return null;
    const plane = C.Plane.fromPointNormal(center, camera.direction);
    return C.IntersectionTests.rayPlane(ray, plane);
  }

  function panTo(position) {
    if (!panStart) return;
    const previous = worldOnCenterPlane(panStart);
    const current = worldOnCenterPlane(position);
    if (!previous || !current) return;
    const movement = C.Cartesian3.subtract(previous, current, new C.Cartesian3());
    camera.position = C.Cartesian3.add(camera.position, movement, camera.position);
    center = C.Cartesian3.add(center, movement, center);
    panStart = C.Cartesian2.clone(position);
    requestRender();
  }

  function beginPan(position) {
    rotateStart = null;
    rotating = false;
    refreshCenter();
    panStart = C.Cartesian2.clone(position);
    panning = true;
    onActivity('pan');
  }

  function endPan() {
    panStart = null;
    if (panning) refreshCenter();
    panning = false;
    onActivity(null);
  }

  function zoomBy(factor) {
    refreshCenter();
    const direction = C.Cartesian3.subtract(center, camera.position, new C.Cartesian3());
    const distance = C.Cartesian3.magnitude(direction);
    if (!Number.isFinite(distance) || distance <= 0) return;
    const nextDistance = C.Math.clamp(distance * factor, .5, Infinity);
    const scale = nextDistance / distance;
    const target = C.Cartesian3.add(
      camera.position,
      C.Cartesian3.multiplyByScalar(direction, 1 - scale, new C.Cartesian3()),
      new C.Cartesian3(),
    );
    camera.position = C.Cartesian3.lerp(camera.position, target, .72, target);
    scheduleCenterRefresh();
    requestRender();
  }

  handler.setInputAction(event => beginRotate(event.position), C.ScreenSpaceEventType.LEFT_DOWN);
  handler.setInputAction(endRotate, C.ScreenSpaceEventType.LEFT_UP);
  handler.setInputAction(event => beginPan(event.position), C.ScreenSpaceEventType.RIGHT_DOWN);
  handler.setInputAction(endPan, C.ScreenSpaceEventType.RIGHT_UP);
  handler.setInputAction(event => beginPan(event.position), C.ScreenSpaceEventType.MIDDLE_DOWN);
  handler.setInputAction(endPan, C.ScreenSpaceEventType.MIDDLE_UP);
  handler.setInputAction(event => {
    if (rotateStart) rotateTo(event.endPosition);
    if (panStart) panTo(event.endPosition);
  }, C.ScreenSpaceEventType.MOUSE_MOVE);
  handler.setInputAction(delta => zoomBy(delta > 0 ? .9 : 1.1), C.ScreenSpaceEventType.WHEEL);
  handler.setInputAction(event => {
    if (event.numberOfTouches === 2) {
      rotateStart = null;
      panStart = null;
      refreshCenter();
    }
  }, C.ScreenSpaceEventType.PINCH_START);
  handler.setInputAction(() => {
    pinch = null;
    refreshCenter();
  }, C.ScreenSpaceEventType.PINCH_END);

  function touchMidpoint(touches) {
    return new C.Cartesian2(
      (touches[0].clientX + touches[1].clientX) / 2,
      (touches[0].clientY + touches[1].clientY) / 2,
    );
  }

  function touchStart(event) {
    if (event.touches.length !== 2) return;
    const midpoint = touchMidpoint(event.touches);
    pinch = {
      distance: Math.hypot(event.touches[0].clientX - event.touches[1].clientX, event.touches[0].clientY - event.touches[1].clientY),
      midpoint,
    };
    refreshCenter();
    panStart = midpoint;
  }

  function touchMove(event) {
    if (!pinch || event.touches.length !== 2) return;
    event.preventDefault();
    const distance = Math.hypot(event.touches[0].clientX - event.touches[1].clientX, event.touches[0].clientY - event.touches[1].clientY);
    const midpoint = touchMidpoint(event.touches);
    if (Math.abs(distance - pinch.distance) > 12) {
      const ratio = pinch.distance / Math.max(distance, 1);
      const direction = C.Cartesian3.subtract(center, camera.position, new C.Cartesian3());
      camera.position = C.Cartesian3.add(
        camera.position,
        C.Cartesian3.multiplyByScalar(direction, 1 - ratio, new C.Cartesian3()),
        camera.position,
      );
      requestRender();
    } else {
      panTo(midpoint);
    }
    pinch = { distance, midpoint };
  }

  function touchEnd(event) {
    if (event.touches.length < 2) {
      pinch = null;
      endPan();
    }
  }

  canvas.addEventListener('touchstart', touchStart, { passive: true });
  canvas.addEventListener('touchmove', touchMove, { passive: false });
  canvas.addEventListener('touchend', touchEnd, { passive: true });
  canvas.addEventListener('touchcancel', touchEnd, { passive: true });

  function flyTo(view = 'orbit') {
    rotateStart = null;
    panStart = null;
    rotating = false;
    panning = false;
    onActivity(null);
    if (!camera.transform.equals(C.Matrix4.IDENTITY)) camera.lookAtTransform(C.Matrix4.IDENTITY);
    const sphere = tileset.boundingSphere;
    const poses = {
      orbit: { pitch: -35, range: 1.75 },
      top: { pitch: -89.4, range: 1.65 },
      front: { pitch: -12, range: 1.75 },
    };
    const pose = poses[view] || poses.orbit;
    const offset = new C.HeadingPitchRange(0, C.Math.toRadians(pose.pitch), sphere.radius * pose.range);
    return new Promise(resolve => {
      const complete = () => {
        center = C.Cartesian3.clone(sphere.center);
        scene.requestRender();
        resolve();
      };
      camera.flyToBoundingSphere(sphere, {
        offset,
        duration: reducedMotion ? 0 : .65,
        complete,
        cancel: complete,
      });
      scene.requestRender();
    });
  }

  return {
    flyTo,
    refreshCenter,
    destroy() {
      destroyed = true;
      cancelAnimationFrame(renderFrame);
      clearTimeout(centerTimer);
      canvas.removeEventListener('touchstart', touchStart);
      canvas.removeEventListener('touchmove', touchMove);
      canvas.removeEventListener('touchend', touchEnd);
      canvas.removeEventListener('touchcancel', touchEnd);
      if (!handler.isDestroyed()) handler.destroy();
      if (!viewer.isDestroyed()) {
        if (!camera.transform.equals(C.Matrix4.IDENTITY)) camera.lookAtTransform(C.Matrix4.IDENTITY);
        defaults.enableInputs = previousInputs;
      }
    },
  };
}
