// include/bsp_view.hpp
#pragma once
#include <lvgl.h>
#include <string>
#include "ESP323248S035.hpp"   // for msecu32_t

namespace bsp {

class View {
public:
  virtual ~View() = default;

  // called once when the view is attached to the root object
  virtual bool init(lv_obj_t *root) = 0;

  // called in loop with elapsed time
  virtual void update(msecu32_t const now) = 0;

  // optional title
  virtual std::string title() = 0;
};

} // namespace bsp
