// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/strings/string_tokenizer.h"
#include "examples/bitmap_uploader/bitmap_uploader.h"
#include "mojo/application/application_runner_chromium.h"
#include "mojo/application/content_handler_factory.h"
#include "mojo/public/c/system/main.h"
#include "mojo/public/cpp/application/application_connection.h"
#include "mojo/public/cpp/application/application_delegate.h"
#include "mojo/public/cpp/application/application_impl.h"
#include "mojo/public/cpp/application/interface_factory_impl.h"
#include "mojo/public/cpp/application/service_provider_impl.h"
#include "mojo/services/public/cpp/view_manager/types.h"
#include "mojo/services/public/cpp/view_manager/view.h"
#include "mojo/services/public/cpp/view_manager/view_manager.h"
#include "mojo/services/public/cpp/view_manager/view_manager_client_factory.h"
#include "mojo/services/public/cpp/view_manager/view_manager_delegate.h"
#include "mojo/services/public/cpp/view_manager/view_observer.h"
#include "mojo/services/public/interfaces/content_handler/content_handler.mojom.h"
#include "mojo/services/public/interfaces/input_events/input_events.mojom.h"
#include "mojo/services/public/interfaces/input_events/input_key_codes.mojom.h"
#include "third_party/pdfium/fpdfsdk/include/fpdf_ext.h"
#include "third_party/pdfium/fpdfsdk/include/fpdfview.h"
#include "v8/include/v8.h"

#define BACKGROUND_COLOR 0xFF888888

namespace mojo {
namespace examples {

namespace {

class EmbedderData {
 public:
  EmbedderData(Shell* shell, View* root) : bitmap_uploader_(root) {
    bitmap_uploader_.Init(shell);
    bitmap_uploader_.SetColor(BACKGROUND_COLOR);
  }

  BitmapUploader& bitmap_uploader() { return bitmap_uploader_; }

 private:
  BitmapUploader bitmap_uploader_;

  DISALLOW_COPY_AND_ASSIGN(EmbedderData);
};

}  // namespace

class PDFView : public ApplicationDelegate,
                public ViewManagerDelegate,
                public ViewObserver {
 public:
  PDFView(URLResponsePtr response)
      : current_page_(0), page_count_(0), doc_(NULL), app_(nullptr) {
    FetchPDF(response.Pass());
  }

  virtual ~PDFView() {
    if (doc_)
      FPDF_CloseDocument(doc_);
    for (auto& roots : embedder_for_roots_) {
      roots.first->RemoveObserver(this);
      delete roots.second;
    }
  }

 private:
  // Overridden from ApplicationDelegate:
  virtual void Initialize(ApplicationImpl* app) override {
    app_ = app;
    view_manager_client_factory_.reset(
        new ViewManagerClientFactory(app->shell(), this));
  }

  virtual bool ConfigureIncomingConnection(
      ApplicationConnection* connection) override {
    connection->AddService(view_manager_client_factory_.get());
    return true;
  }

  // Overridden from ViewManagerDelegate:
  virtual void OnEmbed(ViewManager* view_manager,
                       View* root,
                       ServiceProviderImpl* exported_services,
                       scoped_ptr<ServiceProvider> imported_services) override {
    DCHECK(embedder_for_roots_.find(root) == embedder_for_roots_.end());
    root->AddObserver(this);
    EmbedderData* embedder_data = new EmbedderData(app_->shell(), root);
    embedder_for_roots_[root] = embedder_data;
    DrawBitmap(embedder_data);
  }

  virtual void OnViewManagerDisconnected(ViewManager* view_manager) override {}

  // Overridden from ViewObserver:
  virtual void OnViewBoundsChanged(View* view,
                                   const Rect& old_bounds,
                                   const Rect& new_bounds) override {
    DCHECK(embedder_for_roots_.find(view) != embedder_for_roots_.end());
    DrawBitmap(embedder_for_roots_[view]);
  }

  virtual void OnViewInputEvent(View* view, const EventPtr& event) override {
    DCHECK(embedder_for_roots_.find(view) != embedder_for_roots_.end());
    if (event->key_data &&
        (event->action != EVENT_TYPE_KEY_PRESSED || event->key_data->is_char)) {
      return;
    }
    if ((event->key_data &&
         event->key_data->windows_key_code == KEYBOARD_CODE_DOWN) ||
        (event->wheel_data && event->wheel_data->y_offset < 0)) {
      if (current_page_ < (page_count_ - 1)) {
        current_page_++;
        DrawBitmap(embedder_for_roots_[view]);
      }
    } else if ((event->key_data &&
                event->key_data->windows_key_code == KEYBOARD_CODE_UP) ||
               (event->wheel_data && event->wheel_data->y_offset > 0)) {
      if (current_page_ > 0) {
        current_page_--;
        DrawBitmap(embedder_for_roots_[view]);
      }
    }
  }

  virtual void OnViewDestroyed(View* view) override {
    DCHECK(embedder_for_roots_.find(view) != embedder_for_roots_.end());
    const auto& it = embedder_for_roots_.find(view);
    DCHECK(it != embedder_for_roots_.end());
    delete it->second;
    embedder_for_roots_.erase(it);
    if (embedder_for_roots_.size() == 0)
      ApplicationImpl::Terminate();
  }

  void DrawBitmap(EmbedderData* embedder_data) {
    if (!doc_)
      return;

    FPDF_PAGE page = FPDF_LoadPage(doc_, current_page_);
    int width = static_cast<int>(FPDF_GetPageWidth(page));
    int height = static_cast<int>(FPDF_GetPageHeight(page));

    scoped_ptr<std::vector<unsigned char>> bitmap;
    bitmap.reset(new std::vector<unsigned char>);
    bitmap->resize(width * height * 4);

    FPDF_BITMAP f_bitmap = FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRA,
                                               &(*bitmap)[0], width * 4);
    FPDFBitmap_FillRect(f_bitmap, 0, 0, width, height, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(f_bitmap, page, 0, 0, width, height, 0, 0);
    FPDFBitmap_Destroy(f_bitmap);

    FPDF_ClosePage(page);

    embedder_data->bitmap_uploader().SetBitmap(width, height, bitmap.Pass(),
                                               BitmapUploader::BGRA);
  }

  void FetchPDF(URLResponsePtr response) {
    int content_length = GetContentLength(response->headers);
    scoped_ptr<unsigned char[]> data;
    data_.reset(new unsigned char[content_length]);
    unsigned char* buf = data_.get();
    uint32_t bytes_remaining = content_length;
    uint32_t num_bytes = bytes_remaining;
    while (bytes_remaining > 0) {
      MojoResult result = ReadDataRaw(response->body.get(), buf, &num_bytes,
                                      MOJO_READ_DATA_FLAG_NONE);
      if (result == MOJO_RESULT_SHOULD_WAIT) {
        Wait(response->body.get(), MOJO_HANDLE_SIGNAL_READABLE,
             MOJO_DEADLINE_INDEFINITE);
      } else if (result == MOJO_RESULT_OK) {
        buf += num_bytes;
        num_bytes = bytes_remaining -= num_bytes;
      } else {
        break;
      }
    }

    doc_ = FPDF_LoadMemDocument(data_.get(), content_length, NULL);
    page_count_ = FPDF_GetPageCount(doc_);
  }

  int GetContentLength(const Array<String>& headers) {
    for (size_t i = 0; i < headers.size(); ++i) {
      base::StringTokenizer t(headers[i], ": ;=");
      while (t.GetNext()) {
        if (!t.token_is_delim() && t.token() == "Content-Length") {
          while (t.GetNext()) {
            if (!t.token_is_delim())
              return atoi(t.token().c_str());
          }
        }
      }
    }
    return 0;
  }

  scoped_ptr<unsigned char[]> data_;
  int current_page_;
  int page_count_;
  FPDF_DOCUMENT doc_;
  ApplicationImpl* app_;
  std::map<View*, EmbedderData*> embedder_for_roots_;
  scoped_ptr<ViewManagerClientFactory> view_manager_client_factory_;

  DISALLOW_COPY_AND_ASSIGN(PDFView);
};

class PDFViewer : public ApplicationDelegate,
                  public ContentHandlerFactory::ManagedDelegate {
 public:
  PDFViewer() : content_handler_factory_(this) {
    v8::V8::InitializeICU();
    FPDF_InitLibrary(NULL);
  }

  virtual ~PDFViewer() { FPDF_DestroyLibrary(); }

 private:
  // Overridden from ApplicationDelegate:
  virtual bool ConfigureIncomingConnection(
      ApplicationConnection* connection) override {
    connection->AddService(&content_handler_factory_);
    return true;
  }

  // Overridden from ContentHandlerFactory::ManagedDelegate:
  virtual scoped_ptr<ContentHandlerFactory::HandledApplicationHolder>
  CreateApplication(ShellPtr shell, URLResponsePtr response) override {
    return make_handled_factory_holder(new mojo::ApplicationImpl(
        new PDFView(response.Pass()), shell.PassMessagePipe()));
  }

  ContentHandlerFactory content_handler_factory_;

  DISALLOW_COPY_AND_ASSIGN(PDFViewer);
};

}  // namespace examples
}  // namespace mojo

MojoResult MojoMain(MojoHandle shell_handle) {
  mojo::ApplicationRunnerChromium runner(new mojo::examples::PDFViewer());
  return runner.Run(shell_handle);
}
