#include "cameradialog.h"

#include "ulib/painter.h"
#include "usvg/svgpainter.h"

#include "basics.h"
#include "application.h"
#include "scribbleapp.h"
#include "mainwindow.h"

static const int POLL_MS = 30;

// The newest camera frame, or a message while there is none (starting up, or the camera failed).
class CameraPreviewWidget : public CustomWidget
{
public:
  CameraPreviewWidget() { onPrepareLayout = [](){ return Rect::wh(480, 360); }; }
  void draw(SvgPainter* svgp) const override;

  Image frame{0, 0};
  std::string message;
};

void CameraPreviewWidget::draw(SvgPainter* svgp) const
{
  Painter* painter = svgp->p;
  painter->save();
  painter->fillRect(mBounds, Color::BLACK);
  if(!frame.isNull() && frame.width > 0 && frame.height > 0) {
    real scale = std::min(mBounds.width()/frame.width, mBounds.height()/frame.height);
    real drawWidth = frame.width*scale, drawHeight = frame.height*scale;
    painter->drawImage(Rect::ltwh((mBounds.width() - drawWidth)/2, (mBounds.height() - drawHeight)/2,
        drawWidth, drawHeight), frame);
  }
  else if(!message.empty()) {
    painter->setFillBrush(Color(0xFFC0C0C0));
    painter->setFontSize(16);
    painter->setTextAlign(Painter::AlignHCenter | Painter::AlignVCenter);
    painter->drawText(mBounds.width()/2, mBounds.height()/2, message.c_str());
  }
  painter->restore();
}

CameraDialog::CameraDialog(std::vector<CameraInfo> cameras)
    : PopupDialog(createPopupDialogNode()), cameraList(std::move(cameras))
{
  setTitle(_("Take Photo"));
  previewWidget = new CameraPreviewWidget;
  previewWidget->node->setAttribute("box-anchor", "fill");

  Widget* dialogBody = selectFirst(".body-container");
  dialogBody->node->setAttribute("box-anchor", "fill");
  Widget* column = createColumn();
  column->node->setAttribute("box-anchor", "fill");
  column->addWidget(previewWidget);
  // a laptop with a USB webcam plugged in has two, and the built-in one is rarely the one pointed at paper
  if(cameraList.size() > 1) {
    std::vector<std::string> names;
    for(const CameraInfo& info : cameraList)
      names.push_back(info.name);
    comboCamera = createComboBox(names);
    comboCamera->onChanged = [this](const char*){ openCamera(comboCamera->index()); };
    Widget* cameraRow = createRow({comboCamera});
    cameraRow->setMargins(6, 0);
    column->addWidget(cameraRow);
  }
  dialogBody->addWidget(column);

  cancelBtn = addButton(_("Cancel"), [this](){ finish(CANCELLED); });
  addButton(_("Choose File..."), [this](){ finish(CHOOSE_FILE); });
  acceptBtn = addButton(_("Capture"), [this](){
    if(!previewWidget->frame.isNull())
      finish(ACCEPTED);
  });

  Rect parentBounds = ScribbleApp::win->winBounds();
  setWinBounds(Rect::centerwh(parentBounds.center(),
      std::min(parentBounds.width() - 40, Dim(800)), std::min(parentBounds.height() - 40, Dim(640))));

  openCamera(0);
  Application::gui->setTimer(POLL_MS, previewWidget, [this](){ return pollCamera(); });
}

CameraDialog::~CameraDialog()
{
  Application::gui->removeTimer(previewWidget);
  camera.reset();  // stop the capture thread before the widget its frames go to
}

void CameraDialog::openCamera(int index)
{
  camera.reset();  // release the old device first - some drivers allow only one open stream
  previewWidget->frame = Image(0, 0);
  previewWidget->message = _("Starting camera...");
  acceptBtn->setEnabled(false);
  if(index >= 0 && index < int(cameraList.size()))
    camera = Camera::open(cameraList[index]);
  previewWidget->node->invalidate(true);
}

int CameraDialog::pollCamera()
{
  if(!camera)
    return POLL_MS;
  if(camera->takeFrame(&previewWidget->frame)) {
    acceptBtn->setEnabled(true);
    previewWidget->node->invalidate(true);
  }
  else if(previewWidget->frame.isNull()) {
    std::string error = camera->error();
    if(!error.empty() && previewWidget->message != _(error.c_str())) {
      previewWidget->message = _(error.c_str());
      previewWidget->node->invalidate(true);
    }
  }
  return POLL_MS;
}

Image CameraDialog::takePhoto()
{
  return std::move(previewWidget->frame);
}
