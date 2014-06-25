// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ui/views/cocoa/bridged_native_widget.h"

#import <Cocoa/Cocoa.h>

#include "base/memory/scoped_ptr.h"
#import "testing/gtest_mac.h"
#import "ui/gfx/test/ui_cocoa_test_helper.h"
#import "ui/views/cocoa/bridged_content_view.h"
#include "ui/views/ime/input_method.h"
#include "ui/views/view.h"
#include "ui/views/widget/native_widget_mac.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"

namespace views {
namespace test {

// Provides the |parent| argument to construct a BridgedNativeWidget.
class MockNativeWidgetMac : public NativeWidgetMac {
 public:
  MockNativeWidgetMac(Widget* delegate) : NativeWidgetMac(delegate) {}

  // Expose a reference, so that it can be reset() independently.
  scoped_ptr<BridgedNativeWidget>& bridge() {
    return bridge_;
  }

  // internal::NativeWidgetPrivate:
  virtual void InitNativeWidget(const Widget::InitParams& params) OVERRIDE {
    ownership_ = params.ownership;

    // Usually the bridge gets initialized here. It is skipped to run extra
    // checks in tests, and so that a second window isn't created.
    delegate()->OnNativeWidgetCreated(true);
  }

  virtual void ReorderNativeViews() OVERRIDE {
    // Called via Widget::Init to set the content view. No-op in these tests.
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(MockNativeWidgetMac);
};

// Helper test base to construct a BridgedNativeWidget with a valid parent.
class BridgedNativeWidgetTestBase : public ui::CocoaTest {
 public:
  BridgedNativeWidgetTestBase()
      : widget_(new Widget),
        native_widget_mac_(new MockNativeWidgetMac(widget_.get())) {
  }

  scoped_ptr<BridgedNativeWidget>& bridge() {
    return native_widget_mac_->bridge();
  }

  // Overridden from testing::Test:
  virtual void SetUp() OVERRIDE {
    ui::CocoaTest::SetUp();

    Widget::InitParams params;
    params.native_widget = native_widget_mac_;
    // To control the lifetime without an actual window that must be closed,
    // tests in this file need to use WIDGET_OWNS_NATIVE_WIDGET.
    params.ownership = Widget::InitParams::WIDGET_OWNS_NATIVE_WIDGET;
    native_widget_mac_->GetWidget()->Init(params);
  }

 protected:
  scoped_ptr<Widget> widget_;
  MockNativeWidgetMac* native_widget_mac_;  // Weak. Owned by |widget_|.
};

class BridgedNativeWidgetTest : public BridgedNativeWidgetTestBase {
 public:
  BridgedNativeWidgetTest();
  virtual ~BridgedNativeWidgetTest();

  // testing::Test:
  virtual void SetUp() OVERRIDE;
  virtual void TearDown() OVERRIDE;

 protected:
  // TODO(tapted): Make this a EventCountView from widget_unittest.cc.
  scoped_ptr<views::View> view_;

 private:
  DISALLOW_COPY_AND_ASSIGN(BridgedNativeWidgetTest);
};

BridgedNativeWidgetTest::BridgedNativeWidgetTest() {
}

BridgedNativeWidgetTest::~BridgedNativeWidgetTest() {
}

void BridgedNativeWidgetTest::SetUp() {
  BridgedNativeWidgetTestBase::SetUp();

  view_.reset(new views::View);
  base::scoped_nsobject<NSWindow> window([test_window() retain]);

  EXPECT_FALSE([window delegate]);
  bridge()->Init(window, Widget::InitParams());

  // The delegate should exist before setting the root view.
  EXPECT_TRUE([window delegate]);
  bridge()->SetRootView(view_.get());

  [test_window() makePretendKeyWindowAndSetFirstResponder:bridge()->ns_view()];
}

void BridgedNativeWidgetTest::TearDown() {
  view_.reset();
  BridgedNativeWidgetTestBase::TearDown();
}

// The TEST_VIEW macro expects the view it's testing to have a superview. In
// these tests, the NSView bridge is a contentView, at the root. These mimic
// what TEST_VIEW usually does.
TEST_F(BridgedNativeWidgetTest, BridgedNativeWidgetTest_TestViewAddRemove) {
  base::scoped_nsobject<BridgedContentView> view([bridge()->ns_view() retain]);
  EXPECT_NSEQ([test_window() contentView], view);
  EXPECT_NSEQ(test_window(), [view window]);

  // The superview of a contentView is an NSNextStepFrame.
  EXPECT_TRUE([view superview]);
  EXPECT_TRUE([view hostedView]);

  // Destroying the C++ bridge should remove references to any C++ objects in
  // the ObjectiveC object, and remove it from the hierarchy.
  bridge().reset();
  EXPECT_FALSE([view hostedView]);
  EXPECT_FALSE([view superview]);
  EXPECT_FALSE([view window]);
  EXPECT_FALSE([test_window() contentView]);
  EXPECT_FALSE([test_window() delegate]);
}

TEST_F(BridgedNativeWidgetTest, BridgedNativeWidgetTest_TestViewDisplay) {
  [bridge()->ns_view() display];
}

// Test that resizing the window resizes the root view appropriately.
TEST_F(BridgedNativeWidgetTest, ViewSizeTracksWindow) {
  const int kTestNewWidth = 400;
  const int kTestNewHeight = 300;

  // |test_window()| is borderless, so these should align.
  NSSize window_size = [test_window() frame].size;
  EXPECT_EQ(view_->width(), static_cast<int>(window_size.width));
  EXPECT_EQ(view_->height(), static_cast<int>(window_size.height));

  // Make sure a resize actually occurs.
  EXPECT_NE(kTestNewWidth, view_->width());
  EXPECT_NE(kTestNewHeight, view_->height());

  [test_window() setFrame:NSMakeRect(0, 0, kTestNewWidth, kTestNewHeight)
                  display:NO];
  EXPECT_EQ(kTestNewWidth, view_->width());
  EXPECT_EQ(kTestNewHeight, view_->height());
}

TEST_F(BridgedNativeWidgetTest, CreateInputMethod) {
  scoped_ptr<views::InputMethod> input_method(bridge()->CreateInputMethod());
  EXPECT_TRUE(input_method);
}

TEST_F(BridgedNativeWidgetTest, GetHostInputMethod) {
  EXPECT_TRUE(bridge()->GetHostInputMethod());
}

// A simpler test harness for testing initialization flows.
typedef BridgedNativeWidgetTestBase BridgedNativeWidgetInitTest;

// Test that BridgedNativeWidget remains sane if Init() is never called.
TEST_F(BridgedNativeWidgetInitTest, InitNotCalled) {
  EXPECT_FALSE(bridge()->ns_view());
  EXPECT_FALSE(bridge()->ns_window());
  bridge().reset();
}

// Test attaching to a parent window that is not a NativeWidgetMac. When the
// parent is a NativeWidgetMac, that is covered in widget_unittest.cc by
// WidgetOwnershipTest.Ownership_ViewsNativeWidgetOwnsWidget*.
TEST_F(BridgedNativeWidgetInitTest, ParentWindowNotNativeWidgetMac) {
  Widget::InitParams params;
  params.parent = [test_window() contentView];
  EXPECT_EQ(0u, [[test_window() childWindows] count]);

  base::scoped_nsobject<NSWindow> child_window(
      [[NSWindow alloc] initWithContentRect:NSMakeRect(50, 50, 400, 300)
                                  styleMask:NSBorderlessWindowMask
                                    backing:NSBackingStoreBuffered
                                      defer:NO]);
  [child_window setReleasedWhenClosed:NO];  // Owned by scoped_nsobject.

  EXPECT_FALSE([child_window parentWindow]);
  bridge()->Init(child_window, params);

  EXPECT_EQ(1u, [[test_window() childWindows] count]);
  EXPECT_EQ(test_window(), [bridge()->ns_window() parentWindow]);
  bridge().reset();
  EXPECT_EQ(0u, [[test_window() childWindows] count]);
}

}  // namespace test
}  // namespace views
