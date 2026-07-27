//=========================================================
//  MusE
//  Linux Music Editor
//    $Id: scrollscale.h,v 1.2.2.3 2009/11/04 17:43:26 lunar_shuttle Exp $
//  (C) Copyright 1999 Werner Schweer (ws@seh.de)
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; version 2 of
//  the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//
//=========================================================

#ifndef __SCROLLSCALE_H__
#define __SCROLLSCALE_H__

#include <QSlider>

class QBoxLayout;
class QLabel;
class QResizeEvent;
class QScrollBar;
class QToolButton;
class QTimer;

namespace MusEGui {

//---------------------------------------------------------
//   ScrollScale
//   A scrollbar and a zoom slider in one widget - the strip along the bottom (or
//   side) of the arranger and the editors. Two child widgets do the work:
//     scroll : a QScrollBar, the position. Its value is in PIXELS.
//     scale  : a QSlider, the zoom. Published as scaleChanged() and applied by the
//              canvas as setXMag()/setYMag().
//
//   scaleVal uses the same sign convention as View's xmag/ymag, and it is the
//   reason for the '< 1' branches all over this file:
//     scaleVal >= 1 : zoomed IN  - scaleVal pixels per virtual unit
//     scaleVal <  1 : zoomed OUT - (-scaleVal) virtual units per pixel
//   mag2scale()/scale2mag() convert between that and the slider's linear position,
//   logarithmically, so the zoom feels even across the range.
//
//   Two coordinate spaces again, hence two accessor pairs:
//     pos()    / setPos()    - PIXELS, what the scrollbar holds
//     offset() / setOffset() - VIRTUAL units, via pos2offset()/offset2pos()
//   setScale() has to carry the position through the OLD scale into an offset and
//   back out through the NEW one, which is why it starts with 'int off = offset();'.
//
//   Signals, and who listens:
//     scrollChanged() -> the canvas's setXPos()/setYPos(), plus the ruler and the
//                        track list. This is the cascade that actually moves the
//                        view, and it is rate limited while the handle is being
//                        dragged - see scrollValueChanged().
//     scaleChanged()  -> the canvas's setXMag()/setYMag().
//   setPosSilent() is for callers that only need to update the handle because the
//   view is already where they are putting it - the wheel-scroll animation.
//---------------------------------------------------------

class ScrollScale : public QWidget {
      Q_OBJECT
    
      QSlider* scale;
      QScrollBar* scroll;
      int minVal, maxVal;
      int scaleVal, scaleMin, scaleMax;
      bool showMagFlag;
      QBoxLayout* box;
      bool noScale;
      bool pageButtons;
      int _page;
      int _pages;
      QToolButton* up;
      QToolButton* down;
      QLabel* pageNo;
      bool invers;
      double logbase;
      QToolButton *scaleUp, *scaleDown;

      // Rate limiting for scrollChanged() while the user drags the handle.
      // _pendingScrollVal < 0 means "nothing held back". See scrollValueChanged().
      QTimer* _scrollThrottleTimer;
      int _pendingScrollVal;

      virtual void resizeEvent(QResizeEvent*);

      void emitPendingScroll();

   private slots:
      void pageUp();
      void pageDown();
      // Everything downstream of scrollChanged() - canvas repaint, ruler, track
      //  list - runs synchronously on the same thread that has to repaint THIS
      //  widget. At mouse report rates (125 Hz for a plain mouse, up to 1000 Hz)
      //  that starves our own paint event and the handle visibly trails the
      //  cursor. So while the handle is held down the cascade is rate limited;
      //  the handle itself keeps following the mouse at full rate because Qt
      //  paints it independently of scrollChanged().
      void scrollValueChanged(int val);
      void scrollThrottleTick();
      void scrollSliderReleased();

   public slots:
      void setPos(unsigned);
      // Like setPos(), but does not emit scrollChanged() - for callers
      // that are only updating this widget's own displayed position and
      // are driving whatever else needs to follow (e.g. the canvas)
      // through some other, separate mechanism instead.
      void setPosSilent(unsigned);
      void setPosNoLimit(unsigned); 
      void setMag(int val, int pos_offset = 0);
      void setOffset(int val);
      void setScale(int val, int pos_offset = 0);
      void stepScale(bool up);

   signals:
      void scaleChanged(int);
      void scrollChanged(int);
      void newPage(int);
      // True when the zoom (scale) slider is pressed, false when released.
      // Connect to Canvas::setScrollAnimBlocked(): the pixel mapping changes on
      // every step of the drag, so no scroll animation may run across it.
      void scaleDragStateChanged(bool dragging);

   public:
      ScrollScale(int, int, int, int max, Qt::Orientation,
         QWidget*, int min = 0, bool i=false, double vv = 10.0);
      int xmag() const      { return scale->value(); }
      // Whether the zoom (scale) slider is currently held down by the user.
      bool isScaleDragging() const { return scale->isSliderDown(); }
      void setXmag(int val) { scale->setValue(val); }
      void setRange(int, int);
      void showMag(bool);
      void setNoScale(bool flag) { noScale = flag; }
      void setPageButtons(bool flag);
      void setPage(int n) { _page = n; }
      int page() const { return _page; }
      int pages() const { return _pages; }
      void setPages(int n);
      int pos() const;
      int mag() const;
      int getScaleValue() const { return scaleVal; }
      void range(int* b, int* e) const { *b = minVal; *e = maxVal; }
      int scaleMinimum() const;
      int scaleMaximum() const;
      void setScaleMinimum(int min);
      void setScaleMaximum(int max);
      void setScaleRange(int min, int max);
      
      int offset() const;
      int pos2offset(int pos) const;
      int offset2pos(int off) const;
      int mag2scale(int mag) const;
      int scale2mag(int scale) const;
      static int getQuickZoomLevel(int mag);
      static int convertQuickZoomLevelToMag(int zoomlvl);
      const static int zoomLevels = 38;
      };

} // namespace MusEGui

#endif

