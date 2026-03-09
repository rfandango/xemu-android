package com.izzy2lost.x1box

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PointF
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import kotlin.math.pow
import kotlin.math.sqrt

class OnScreenController @JvmOverloads constructor(
  context: Context,
  attrs: AttributeSet? = null,
  defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

  private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
  private val buttons = mutableMapOf<Button, ButtonState>()
  private val sticks = mutableMapOf<Stick, StickState>()

  private var menuButtonCenter = PointF(0f, 0f)
  private var menuButtonRadius = 0f
  private var menuButtonPressed = false
  private var menuButtonPointerId = -1

  var onMenuButtonTapped: (() -> Unit)? = null

  private var controllerListener: ControllerListener? = null

  enum class Button {
    A, B, X, Y,
    DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT,
    LEFT_TRIGGER, RIGHT_TRIGGER,
    START, BACK,
    LEFT_STICK_BUTTON, RIGHT_STICK_BUTTON,
    BLACK, WHITE
  }

  enum class Stick {
    LEFT, RIGHT
  }

  data class ButtonState(
    val center: PointF,
    val radius: Float,
    var isPressed: Boolean = false,
    var activePointerId: Int = -1
  )

  data class StickState(
    val center: PointF,
    val radius: Float,
    val deadZone: Float = 0.2f,
    var currentPos: PointF = PointF(0f, 0f),
    var isPressed: Boolean = false,
    var activePointerId: Int = -1
  )

  interface ControllerListener {
    fun onButtonPressed(button: Button)
    fun onButtonReleased(button: Button)
    fun onStickMoved(stick: Stick, x: Float, y: Float)
    fun onStickPressed(stick: Stick)
    fun onStickReleased(stick: Stick)
  }

  init {
    setBackgroundColor(Color.TRANSPARENT)
  }

  fun setControllerListener(listener: ControllerListener) {
    this.controllerListener = listener
  }

  override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
    super.onSizeChanged(w, h, oldw, oldh)
    initializeControls(w, h)
  }

  private fun initializeControls(width: Int, height: Int) {
    val w = width.toFloat()
    val h = height.toFloat()
    
    // Button sizes - made D-pad smaller
    val faceButtonRadius = w * 0.032f
    val dpadButtonRadius = w * 0.025f
    val shoulderButtonRadius = w * 0.034f
    val smallButtonRadius = w * 0.022f
    val stickRadius = w * 0.07f

    // Face buttons (right side) - A, B, X, Y in diamond formation
    val faceButtonCenterX = w * 0.88f
    val faceButtonCenterY = h * 0.55f
    val faceButtonSpacing = w * 0.064f

    buttons[Button.A] = ButtonState(
      PointF(faceButtonCenterX, faceButtonCenterY + faceButtonSpacing),
      faceButtonRadius
    )
    buttons[Button.B] = ButtonState(
      PointF(faceButtonCenterX + faceButtonSpacing, faceButtonCenterY),
      faceButtonRadius
    )
    buttons[Button.X] = ButtonState(
      PointF(faceButtonCenterX - faceButtonSpacing, faceButtonCenterY),
      faceButtonRadius
    )
    buttons[Button.Y] = ButtonState(
      PointF(faceButtonCenterX, faceButtonCenterY - faceButtonSpacing),
      faceButtonRadius
    )

    // D-Pad (bottom left corner) - smaller buttons
    val dpadCenterX = w * 0.12f
    val dpadCenterY = h * 0.83f
    val dpadSpacing = w * 0.045f

    buttons[Button.DPAD_UP] = ButtonState(
      PointF(dpadCenterX, dpadCenterY - dpadSpacing),
      dpadButtonRadius
    )
    buttons[Button.DPAD_DOWN] = ButtonState(
      PointF(dpadCenterX, dpadCenterY + dpadSpacing),
      dpadButtonRadius
    )
    buttons[Button.DPAD_LEFT] = ButtonState(
      PointF(dpadCenterX - dpadSpacing, dpadCenterY),
      dpadButtonRadius
    )
    buttons[Button.DPAD_RIGHT] = ButtonState(
      PointF(dpadCenterX + dpadSpacing, dpadCenterY),
      dpadButtonRadius
    )

    // Trigger buttons - smaller and aligned to top corners
    val shoulderEdgeMargin = shoulderButtonRadius + (w * 0.02f)
    buttons[Button.LEFT_TRIGGER] = ButtonState(
      PointF(shoulderEdgeMargin, shoulderEdgeMargin),
      shoulderButtonRadius
    )
    buttons[Button.RIGHT_TRIGGER] = ButtonState(
      PointF(w - shoulderEdgeMargin, shoulderEdgeMargin),
      shoulderButtonRadius
    )

    // Analog sticks
    sticks[Stick.LEFT] = StickState(
      PointF(w * 0.18f, h * 0.45f),
      stickRadius
    )
    sticks[Stick.RIGHT] = StickState(
      PointF(w * 0.62f, h * 0.82f),
      stickRadius
    )

    // Center buttons - moved near bottom, between D-pad and right stick
    val centerButtonsBaseX = (dpadCenterX + sticks[Stick.RIGHT]!!.center.x) * 0.5f
    val centerButtonsY = h * 0.9f
    val centerButtonSpacing = smallButtonRadius * 3.4f
    buttons[Button.BACK] = ButtonState(
      PointF(centerButtonsBaseX - (centerButtonSpacing * 0.5f), centerButtonsY),
      smallButtonRadius
    )
    buttons[Button.START] = ButtonState(
      PointF(centerButtonsBaseX + (centerButtonSpacing * 0.5f), centerButtonsY),
      smallButtonRadius
    )

    // Black and White buttons - black to the right of white
    val whiteBlackY = h * 0.8f
    val whiteX = w * 0.75f
    val whiteBlackSpacing = smallButtonRadius * 2.6f
    buttons[Button.WHITE] = ButtonState(
      PointF(whiteX, whiteBlackY),
      smallButtonRadius
    )
    buttons[Button.BLACK] = ButtonState(
      PointF(whiteX + whiteBlackSpacing, whiteBlackY),
      smallButtonRadius
    )

    // LS/RS (thumbstick click) as dedicated buttons — separate from the stick
    // circles so they can't be triggered accidentally by joystick movement.
    // LS sits bottom-left of left stick; RS sits bottom-left of right stick.
    buttons[Button.LEFT_STICK_BUTTON] = ButtonState(
      PointF(w * 0.07f, h * 0.53f),
      smallButtonRadius
    )
    buttons[Button.RIGHT_STICK_BUTTON] = ButtonState(
      PointF(w * 0.51f, h * 0.9f),
      smallButtonRadius
    )

    // Menu button — bottom-right corner, opens in-game menu
    menuButtonRadius = smallButtonRadius * 1.25f
    menuButtonCenter = PointF(
      w - menuButtonRadius - w * 0.018f,
      h - menuButtonRadius - h * 0.022f
    )
  }

  override fun onDraw(canvas: Canvas) {
    super.onDraw(canvas)

    // Draw analog sticks
    sticks.forEach { (stick, state) ->
      // Outer circle
      paint.style = Paint.Style.STROKE
      paint.strokeWidth = 4f
      paint.color = Color.argb(100, 255, 255, 255)
      canvas.drawCircle(state.center.x, state.center.y, state.radius, paint)

      // Dead zone circle
      paint.color = Color.argb(50, 255, 255, 255)
      canvas.drawCircle(state.center.x, state.center.y, state.radius * state.deadZone, paint)

      // Stick position
      val stickX = state.center.x + state.currentPos.x * state.radius
      val stickY = state.center.y + state.currentPos.y * state.radius
      
      paint.style = Paint.Style.FILL
      paint.color = if (state.isPressed) {
        Color.argb(200, 100, 150, 255)
      } else {
        Color.argb(150, 200, 200, 200)
      }
      canvas.drawCircle(stickX, stickY, state.radius * 0.4f, paint)
    }

    // Draw buttons
    buttons.forEach { (button, state) ->

      paint.style = Paint.Style.FILL
      paint.color = when {
        state.isPressed -> getButtonPressedColor(button)
        else -> getButtonColor(button)
      }
      canvas.drawCircle(state.center.x, state.center.y, state.radius, paint)

      // Button outline
      paint.style = Paint.Style.STROKE
      paint.strokeWidth = 3f
      paint.color = Color.argb(150, 255, 255, 255)
      canvas.drawCircle(state.center.x, state.center.y, state.radius, paint)

      // Button labels
      paint.style = Paint.Style.FILL
      paint.color = Color.WHITE
      paint.textSize = state.radius * 0.8f
      paint.textAlign = Paint.Align.CENTER
      val label = getButtonLabel(button)
      canvas.drawText(label, state.center.x, state.center.y + state.radius * 0.3f, paint)
    }

    // Draw menu button (bottom-right corner)
    paint.style = Paint.Style.FILL
    paint.color = if (menuButtonPressed) {
      Color.argb(200, 120, 120, 220)
    } else {
      Color.argb(120, 100, 100, 180)
    }
    canvas.drawCircle(menuButtonCenter.x, menuButtonCenter.y, menuButtonRadius, paint)

    paint.style = Paint.Style.STROKE
    paint.strokeWidth = 3f
    paint.color = Color.argb(150, 255, 255, 255)
    canvas.drawCircle(menuButtonCenter.x, menuButtonCenter.y, menuButtonRadius, paint)

    paint.style = Paint.Style.FILL
    paint.color = Color.WHITE
    paint.textSize = menuButtonRadius * 1.0f
    paint.textAlign = Paint.Align.CENTER
    canvas.drawText("☰", menuButtonCenter.x, menuButtonCenter.y + menuButtonRadius * 0.35f, paint)
  }

  private fun getButtonColor(button: Button): Int {
    return when (button) {
      Button.A -> Color.argb(150, 100, 200, 100)
      Button.B -> Color.argb(150, 200, 100, 100)
      Button.X -> Color.argb(150, 100, 150, 255)
      Button.Y -> Color.argb(150, 255, 255, 100)
      Button.BLACK -> Color.argb(150, 50, 50, 50)
      Button.WHITE -> Color.argb(150, 220, 220, 220)
      else -> Color.argb(120, 150, 150, 150)
    }
  }

  private fun getButtonPressedColor(button: Button): Int {
    return when (button) {
      Button.A -> Color.argb(255, 100, 255, 100)
      Button.B -> Color.argb(255, 255, 100, 100)
      Button.X -> Color.argb(255, 100, 150, 255)
      Button.Y -> Color.argb(255, 255, 255, 100)
      Button.BLACK -> Color.argb(255, 80, 80, 80)
      Button.WHITE -> Color.argb(255, 255, 255, 255)
      else -> Color.argb(200, 200, 200, 200)
    }
  }

  private fun getButtonLabel(button: Button): String {
    return when (button) {
      Button.A -> "A"
      Button.B -> "B"
      Button.X -> "X"
      Button.Y -> "Y"
      Button.DPAD_UP -> "↑"
      Button.DPAD_DOWN -> "↓"
      Button.DPAD_LEFT -> "←"
      Button.DPAD_RIGHT -> "→"
      Button.LEFT_TRIGGER -> "LT"
      Button.RIGHT_TRIGGER -> "RT"
      Button.START -> "▶"
      Button.BACK -> "◀"
      Button.BLACK -> "BK"
      Button.WHITE -> "WH"
      Button.LEFT_STICK_BUTTON -> "LS"
      Button.RIGHT_STICK_BUTTON -> "RS"
      else -> ""
    }
  }

  override fun onTouchEvent(event: MotionEvent): Boolean {
    val pointerIndex = event.actionIndex
    val pointerId = event.getPointerId(pointerIndex)
    val x = event.getX(pointerIndex)
    val y = event.getY(pointerIndex)

    when (event.actionMasked) {
      MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
        handleTouchDown(x, y, pointerId)
      }
      MotionEvent.ACTION_MOVE -> {
        for (i in 0 until event.pointerCount) {
          handleTouchMove(
            event.getX(i),
            event.getY(i),
            event.getPointerId(i)
          )
        }
      }
      MotionEvent.ACTION_POINTER_UP -> {
        handleTouchUp(pointerId)
      }
      MotionEvent.ACTION_UP -> {
        handleTouchUp(pointerId)
        // Last finger lifted — release everything as a safety net in case
        // pointer ID tracking got confused by multi-touch or system gestures.
        handleCancel()
      }
      MotionEvent.ACTION_CANCEL -> {
        handleCancel()
      }
    }

    invalidate()
    return true
  }

  private fun handleTouchDown(x: Float, y: Float, pointerId: Int) {
    // Check menu button first (UI button, not a controller input)
    if (menuButtonPointerId == -1 && isPointInCircle(x, y, menuButtonCenter, menuButtonRadius)) {
      menuButtonPressed = true
      menuButtonPointerId = pointerId
      return
    }

    // Check sticks first
    sticks.forEach { (stick, state) ->
      if (state.activePointerId == -1 && isPointInCircle(x, y, state.center, state.radius)) {
        state.activePointerId = pointerId
        updateStickPosition(stick, state, x, y)
        return
      }
    }

    // Check buttons
    buttons.forEach { (button, state) ->
      if (state.activePointerId == -1 && isPointInCircle(x, y, state.center, state.radius)) {
        pressButton(button, state, pointerId)
        return
      }
    }
  }

  private fun handleTouchMove(x: Float, y: Float, pointerId: Int) {
    if (menuButtonPointerId == pointerId) {
      menuButtonPressed = isPointInCircle(x, y, menuButtonCenter, menuButtonRadius)
      return
    }

    sticks.forEach { (stick, state) ->
      if (state.activePointerId == pointerId) {
        updateStickPosition(stick, state, x, y)
        return
      }
    }

    val activeButton = buttons.entries.firstOrNull { it.value.activePointerId == pointerId }
    if (activeButton != null) {
      val (button, state) = activeButton
      if (isPointInCircle(x, y, state.center, state.radius)) {
        return
      }
      releaseButton(button, state)
      // Only slide to a new button when this pointer was already tracking one.
      // This prevents other pointers from stealing a just-released button in the
      // same ACTION_MOVE batch, which would cause triggers (LT/RT) to get stuck.
      val hoveredButton = buttons.entries.firstOrNull { (_, s) ->
        s.activePointerId == -1 && isPointInCircle(x, y, s.center, s.radius)
      }
      if (hoveredButton != null) {
        val (newButton, newState) = hoveredButton
        pressButton(newButton, newState, pointerId)
      }
    }
  }

  private fun handleTouchUp(pointerId: Int) {
    // Release menu button (fire on up = tap semantics)
    if (menuButtonPointerId == pointerId) {
      if (menuButtonPressed) {
        onMenuButtonTapped?.invoke()
      }
      menuButtonPressed = false
      menuButtonPointerId = -1
    }

    // Release sticks
    sticks.forEach { (stick, state) ->
      if (state.activePointerId == pointerId) {
        state.activePointerId = -1
        state.currentPos = PointF(0f, 0f)
        controllerListener?.onStickMoved(stick, 0f, 0f)
        if (state.isPressed) {
          state.isPressed = false
          controllerListener?.onStickReleased(stick)
        }
      }
    }

    // Release buttons
    buttons.forEach { (button, state) ->
      if (state.activePointerId == pointerId) {
        releaseButton(button, state)
      }
    }
  }

  private fun handleCancel() {
    if (menuButtonPointerId != -1) {
      menuButtonPressed = false
      menuButtonPointerId = -1
    }

    sticks.forEach { (stick, state) ->
      if (state.activePointerId != -1) {
        state.activePointerId = -1
        state.currentPos = PointF(0f, 0f)
        controllerListener?.onStickMoved(stick, 0f, 0f)
        if (state.isPressed) {
          state.isPressed = false
          controllerListener?.onStickReleased(stick)
        }
      }
    }

    buttons.forEach { (button, state) ->
      if (state.isPressed) {
        releaseButton(button, state)
      }
    }
  }

  fun resetAllInputs() {
    handleCancel()
    invalidate()
  }

  override fun onDetachedFromWindow() {
    resetAllInputs()
    super.onDetachedFromWindow()
  }

  override fun onVisibilityChanged(changedView: View, visibility: Int) {
    super.onVisibilityChanged(changedView, visibility)
    if (changedView === this && visibility != View.VISIBLE) {
      resetAllInputs()
    }
  }

  private fun pressButton(button: Button, state: ButtonState, pointerId: Int) {
    state.isPressed = true
    state.activePointerId = pointerId
    controllerListener?.onButtonPressed(button)
  }

  private fun releaseButton(button: Button, state: ButtonState) {
    state.isPressed = false
    state.activePointerId = -1
    controllerListener?.onButtonReleased(button)
  }

  private fun updateStickPosition(stick: Stick, state: StickState, x: Float, y: Float) {
    val dx = x - state.center.x
    val dy = y - state.center.y
    val distance = sqrt(dx.pow(2) + dy.pow(2))

    if (distance > state.radius) {
      // Clamp to circle boundary
      state.currentPos.x = (dx / distance)
      state.currentPos.y = (dy / distance)
    } else {
      // Normalize to -1..1 range
      state.currentPos.x = dx / state.radius
      state.currentPos.y = dy / state.radius
    }

    // Apply dead zone
    val magnitude = sqrt(state.currentPos.x.pow(2) + state.currentPos.y.pow(2))
    if (magnitude < state.deadZone) {
      state.currentPos.x = 0f
      state.currentPos.y = 0f
    }

    controllerListener?.onStickMoved(stick, state.currentPos.x, state.currentPos.y)
  }

  private fun isPointInCircle(x: Float, y: Float, center: PointF, radius: Float): Boolean {
    val dx = x - center.x
    val dy = y - center.y
    return sqrt(dx.pow(2) + dy.pow(2)) <= radius
  }

  fun setVisibility(visible: Boolean) {
    visibility = if (visible) View.VISIBLE else View.GONE
  }
}
