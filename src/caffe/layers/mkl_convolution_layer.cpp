#ifdef MKL2017_SUPPORTED
#include <algorithm>
#include <cstdlib>
#include <vector>

#include "caffe/filler.hpp"
#include "caffe/layer.hpp"
#include "caffe/layers/mkl_layers.hpp"
#include "mkl_service.h"

static int getMKLBuildDate() {
  static int build = 0;
  if (build == 0) {
    MKLVersion v;
    mkl_get_version(&v);
    build = atoi(v.Build);
  }
  return build;
}

namespace caffe {
template <typename Dtype>
MKLConvolutionLayer<Dtype>::MKLConvolutionLayer(
  const LayerParameter& param)
      : ConvolutionLayer<Dtype>(param),
        fwd_bottom_data(new MKLData<Dtype>()),
        fwd_top_data(new MKLData<Dtype>()),
        fwd_filter_data(new MKLData<Dtype>()),
        fwd_bias_data(new MKLData<Dtype>()),
        convolutionFwd(NULL),
        bwdd_top_diff(new MKLDiff<Dtype>()),
        bwdd_bottom_diff(new MKLDiff<Dtype>()),
        bwdd_filter_data(new MKLData<Dtype>()),
        convolutionBwdData(static_cast<dnnPrimitive_t>(NULL)),
        bwdf_top_diff(new MKLDiff<Dtype>()),
        bwdf_filter_diff(new MKLDiff<Dtype>()),
        bwdf2fwd_filter_diff(new MKLDiff<Dtype>()),
        bwdf_bottom_data(new MKLData<Dtype>()),
        convolutionBwdFilter(static_cast<dnnPrimitive_t>(NULL)),
        bwdb_top_diff(new MKLDiff<Dtype>()),
        bwdb_bias_diff(new MKLDiff<Dtype>()),
        convolutionBwdBias(static_cast<dnnPrimitive_t>(NULL)) {}

template <typename Dtype>
void MKLConvolutionLayer<Dtype>::compute_output_shape() {
  ConvolutionLayer<Dtype>::compute_output_shape();
  this->height_out_ = (this->height_ + 2 * this->pad_h_ - this->kernel_h_)
      / this->stride_h_ + 1;
  this->width_out_ = (this->width_ + 2 * this->pad_w_ - this->kernel_w_)
      / this->stride_w_ + 1;
}

template <typename Dtype>
MKLConvolutionLayer<Dtype>::~MKLConvolutionLayer() {
    dnnDelete<Dtype>(convolutionFwd);
    dnnDelete<Dtype>(convolutionBwdData);
    dnnDelete<Dtype>(convolutionBwdFilter);
    if (this->bias_term_)
        dnnDelete<Dtype>(convolutionBwdBias);
}

template <typename Dtype>
void MKLConvolutionLayer<Dtype>::LayerSetUp(
      const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  ConvolutionLayer<Dtype>::LayerSetUp(bottom, top);

  this->width_ = bottom[0]->width();
  this->height_ = bottom[0]->height();
  this->num_ = bottom[0]->num();

  // TODO: clean up this
  kernel_w_ = this->kernel_shape_.cpu_data()[0];
  kernel_h_ = this->kernel_shape_.cpu_data()[1];
  stride_w_ = this->stride_.cpu_data()[0];
  stride_h_ = this->stride_.cpu_data()[1];
  pad_w_ = this->pad_.cpu_data()[0];
  pad_h_ = this->pad_.cpu_data()[1];

  this->bottom_shape_ = &bottom[0]->shape();
  compute_output_shape();
  int status;
  size_t n, g;
  size_t iw, ih, ic;
  size_t ow, oh, oc;
  size_t kw, kh; /* filter */
  size_t dimension = 4;

  g  = std::max(this->group_, 1);
  n  = this->num_;
  iw = this->width_;
  ih = this->height_;
  ic = this->channels_;

  ow = this->width_out_;
  oh = this->height_out_;
  oc = this->num_output_;

  kw = this->kernel_w_;
  kh = this->kernel_h_;

  size_t bdata_sizes[4] = {iw, ih, ic, n};
  size_t bdata_strides[4] = {1, iw, iw*ih, iw*ih*ic};

  /* starting with MKL 2017 Gold in case of groups filter layout
   * becomes 5D, i.e. groups become a separate dimension */
  size_t g_mkl2017 = g;
  size_t f_dimension = dimension + (g != 1);
  if (getMKLBuildDate() < 20160701) {
      g_mkl2017 = 1;
      f_dimension = dimension;
  }

  size_t fdata_sizes[5] = {kw, kh, ic/g, oc/g_mkl2017, g_mkl2017};
  size_t fdata_strides[5]  = {1, kw, kw*kh, kw*kh*ic/g, kw*kh*ic/g*oc/g};

  size_t bias_sizes[1] = {oc};
  size_t bias_strides[1] = {1};

  size_t tdata_sizes[4] = {ow, oh, oc, n};
  size_t tdata_strides[4]  = {1, ow, ow*oh, ow*oh*oc};

  size_t convolutionStrides[2] = {this->stride_w_, this->stride_h_};
  int    inputOffset[2] = {-this->pad_w_, -this->pad_h_};

  // Names are for debugging purposes only.
  fwd_bottom_data ->name = "fwd_bottom_data   @ " + this->layer_param_.name();
  fwd_top_data    ->name = "fwd_top_data      @ " + this->layer_param_.name();
  fwd_filter_data ->name = "fwd_filter_data   @ " + this->layer_param_.name();
  fwd_bias_data   ->name = "fwd_bias_data     @ " + this->layer_param_.name();
  bwdd_top_diff   ->name = "bwdd_top_diff     @ " + this->layer_param_.name();
  bwdd_bottom_diff->name = "bwdd_bottom_diff  @ " + this->layer_param_.name();
  bwdd_filter_data->name = "bwdd_filter_data  @ " + this->layer_param_.name();
  bwdf_top_diff   ->name = "bwdf_top_diff     @ " + this->layer_param_.name();
  bwdf_bottom_data->name = "bwdf_bottom_data  @ " + this->layer_param_.name();
  bwdf_filter_diff->name = "bwdf_filter_diff  @ " + this->layer_param_.name();
  bwdf2fwd_filter_diff->name =
                       "bwdf2fwd_filter_diff  @ " + this->layer_param_.name();
  bwdb_top_diff   ->name = "bwdb_top_diff     @ " + this->layer_param_.name();
  bwdb_bias_diff  ->name = "bwdb_bias_diff    @ " + this->layer_param_.name();

  if (this->bias_term_) {
    status = dnnGroupsConvolutionCreateForwardBias<Dtype>(
      &convolutionFwd,
      NULL,
      dnnAlgorithmConvolutionDirect,
      g,
      dimension,
      bdata_sizes,
      tdata_sizes,
      fdata_sizes,
      convolutionStrides,
      inputOffset,
      dnnBorderZeros);
  } else {
    status = dnnGroupsConvolutionCreateForward<Dtype>(
      &convolutionFwd,
      NULL,
      dnnAlgorithmConvolutionDirect,
      g,
      dimension,
      bdata_sizes,
      tdata_sizes,
      fdata_sizes,
      convolutionStrides,
      inputOffset,
      dnnBorderZeros);
  }

  CHECK_EQ(status, 0)
          << "Failed dnnCreateConvolution<Dtype>(dnnForward) with status "
          << status << "\n";

  fwd_bottom_data->create_layouts(convolutionFwd, dnnResourceSrc, dimension,
                                  bdata_sizes, bdata_strides);
  fwd_top_data   ->create_layouts(convolutionFwd, dnnResourceDst, dimension,
                                  tdata_sizes, tdata_strides);
  fwd_filter_data->create_layouts(convolutionFwd, dnnResourceFilter, f_dimension,
                                  fdata_sizes, fdata_strides);

  if (this->bias_term_)
    fwd_bias_data->create_layouts(convolutionFwd, dnnResourceBias, 1,
                                  bias_sizes, bias_strides);

/*
 * Backward by data layer setup
 */
  status = dnnGroupsConvolutionCreateBackwardData<Dtype>(
    &convolutionBwdData,
    NULL,
    dnnAlgorithmConvolutionDirect,
    g,
    dimension,
    bdata_sizes,
    tdata_sizes,
    fdata_sizes,
    convolutionStrides,
    inputOffset,
    dnnBorderZeros);
  CHECK_EQ(status, 0)
          << "Failed dnnConvolutionCreateBackwardData with status "
          << status << "\n";

  bwdd_bottom_diff->create_layouts(convolutionBwdData, dnnResourceDiffSrc,
                                   dimension, bdata_sizes, bdata_strides);
  bwdd_top_diff   ->create_layouts(convolutionBwdData, dnnResourceDiffDst,
                                   dimension, tdata_sizes, tdata_strides);
  bwdd_filter_data->create_layouts(convolutionBwdData, dnnResourceFilter,
                                   f_dimension, fdata_sizes, fdata_strides);

/*
 * Backward by filter layer setup
 */
  status = dnnGroupsConvolutionCreateBackwardFilter<Dtype>(
    &convolutionBwdFilter,
    NULL,
    dnnAlgorithmConvolutionDirect,
    g,
    dimension,
    bdata_sizes,
    tdata_sizes,
    fdata_sizes,
    convolutionStrides,
    inputOffset,
    dnnBorderZeros);
  CHECK_EQ(status, 0)
          << "Failed dnnConvolutionCreateBackwardFilter with status "
          << status << "\n";

  bwdf_bottom_data->create_layouts(convolutionBwdFilter, dnnResourceSrc,
                                   dimension, bdata_sizes, bdata_strides);
  bwdf_top_diff   ->create_layouts(convolutionBwdFilter, dnnResourceDiffDst,
                                   dimension, tdata_sizes, tdata_strides);
  bwdf_filter_diff->create_layouts(convolutionFwd, dnnResourceFilter,
                                   f_dimension, fdata_sizes, fdata_strides);


  // Note: this caused some trouble for older MKL
  if (getMKLBuildDate() > 20160701) {
    // bwdf2fwd_filter_diff:
    // layout_int = internal layout of weight diff on backward filter convolution,
    // layout_usr = internal layout of weight on forward convolution
    bwdf2fwd_filter_diff->create_internal_layout(convolutionBwdFilter,
            dnnResourceDiffFilter);
    status = dnnLayoutCreateFromPrimitive<Dtype>(
            &bwdf2fwd_filter_diff->layout_usr, convolutionFwd, dnnResourceFilter);
    CHECK_EQ(status, 0) << "Failed dnnLayoutCreateFromPrimitive with status "
            << status << "\n";

    bwdf2fwd_filter_diff->create_conversions();
  }

/*
 * Backward by bias layer setup
 */
  if (this->bias_term_) {
    status = dnnGroupsConvolutionCreateBackwardBias<Dtype>(
      &convolutionBwdBias,
      NULL,
      dnnAlgorithmConvolutionDirect,
      g,
      dimension,
      tdata_sizes);
    CHECK_EQ(status, 0)
            << "Failed dnnConvolutionCreateBackwardBias with status "
            << status << "\n";

    bwdb_top_diff->create_layouts(convolutionBwdBias, dnnResourceDiffDst,
                                  dimension, tdata_sizes, tdata_strides);
    bwdb_bias_diff->create_layouts(convolutionBwdBias, dnnResourceDiffBias, 1,
                                   bias_sizes, bias_strides);
  }
}

template <typename Dtype>
void MKLConvolutionLayer<Dtype>::Reshape(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  BaseConvolutionLayer<Dtype>::Reshape(bottom, top);

  if (this->width_ == bottom[0]->width() &&
      this->height_ == bottom[0]->height() &&
      this->channels_ == bottom[0]->channels() &&
      this->num_ == bottom[0]->num())
    return;

  // Free MKL primitives
  dnnDelete<Dtype>(convolutionFwd);
  dnnDelete<Dtype>(convolutionBwdData);

  // Reinit layer params
  this->width_ = bottom[0]->width();
  this->height_ = bottom[0]->height();
  this->num_ = bottom[0]->num();

  this->bottom_shape_ = &bottom[0]->shape();
  compute_output_shape();
  int status;
  size_t n, g;
  size_t ic, oc, kw, kh;
  size_t dimension = 4;

  g  = this->group_;
  n  = this->num_;
  ic = this->channels_;
  kw = this->kernel_w_;
  kh = this->kernel_h_;

  oc = this->num_output_;

  size_t bdata_sizes[4] = {this->width_, this->height_, ic, n};

  /* starting with MKL 2017 Gold in case of groups filter layout
   * becomes 5D, i.e. groups become a separate dimension */
  size_t g_mkl2017 = g;
  size_t f_dimension = dimension + (g != 1);
  if (getMKLBuildDate() < 20160701) {
      g_mkl2017 = 1;
      f_dimension = dimension;
  }

  size_t fdata_sizes[5] = {kw, kh, ic/g, oc/g_mkl2017, g_mkl2017};

  size_t tdata_sizes[4] = {this->width_out_, this->height_out_, oc, n};

  size_t convolutionStrides[2] = {this->stride_w_, this->stride_h_};
  int    inputOffset[2] = {-this->pad_w_, -this->pad_h_};

  // Recreate MKL primitives
  if (this->bias_term_) {
    status = dnnGroupsConvolutionCreateForwardBias<Dtype>(
      &convolutionFwd,
      NULL,
      dnnAlgorithmConvolutionDirect,
      g,
      dimension,
      bdata_sizes,
      tdata_sizes,
      fdata_sizes,
      convolutionStrides,
      inputOffset,
      dnnBorderZeros);
  } else {
    status = dnnGroupsConvolutionCreateForward<Dtype>(
      &convolutionFwd,
      NULL,
      dnnAlgorithmConvolutionDirect,
      g,
      dimension,
      bdata_sizes,
      tdata_sizes,
      fdata_sizes,
      convolutionStrides,
      inputOffset,
      dnnBorderZeros);
  }
  CHECK_EQ(status, 0)
          << "Failed dnnCreateConvolution<Dtype>(dnnForward) with status "
          << status << "\n";

  status = dnnGroupsConvolutionCreateBackwardData<Dtype>(
    &convolutionBwdData,
    NULL,
    dnnAlgorithmConvolutionDirect,
    g,
    dimension,
    bdata_sizes,
    tdata_sizes,
    fdata_sizes,
    convolutionStrides,
    inputOffset,
    dnnBorderZeros);
  CHECK_EQ(status, 0)
          << "Failed dnnConvolutionCreateBackwardData with status "
          << status << "\n";
}

template <typename Dtype>
void MKLConvolutionLayer<Dtype>::Forward_cpu(
  const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  int status;
  size_t n, g;
  size_t iw, ih, ic;
  size_t ow, oh, oc;

  g  = this->group_;
  n  = this->num_;
  iw = this->width_;
  ih = this->height_;
  ic = this->channels_/g;

  CHECK(bottom[0]->width()    == iw &&
        bottom[0]->height()   == ih &&
        bottom[0]->channels() == ic*g &&
        bottom[0]->num()      == n)
          << "Inclompatible shape of bottom with layer";

  ow = this->width_out_;
  oh = this->height_out_;
  oc = this->num_output_/g;
  CHECK(top[0]->width()    == ow &&
        top[0]->height()   == oh &&
        top[0]->channels() == oc*g &&
        top[0]->num()      == n) << "Inclompatible shape of bottom with layer";


  void *res_convolutionFwd[dnnResourceNumber];
  res_convolutionFwd[dnnResourceSrc] =
    fwd_bottom_data->get_converted_prv(bottom[0], false);
  res_convolutionFwd[dnnResourceFilter] =
    fwd_filter_data->get_converted_prv(this->blobs_[0].get(), true);
  if (this->bias_term_) {
    res_convolutionFwd[dnnResourceBias] =
      fwd_bias_data  ->get_converted_prv(this->blobs_[1].get(), true);
  }

  if (fwd_top_data->conversion_needed()) {
    top[0]->set_prv_data_descriptor(fwd_top_data);
    res_convolutionFwd[dnnResourceDst] =
            reinterpret_cast<void *>(top[0]->mutable_prv_data());
  } else {
    res_convolutionFwd[dnnResourceDst] = top[0]->mutable_cpu_data();
  }
  status = dnnExecute<Dtype>(convolutionFwd, res_convolutionFwd);
  CHECK_EQ(status, 0) << "Forward convolution failed with status " << status;
}

template <typename Dtype>
void MKLConvolutionLayer<Dtype>::Backward_cpu(
  const vector<Blob<Dtype>*>& top, const vector<bool>& propagate_down,
  const vector<Blob<Dtype>*>& bottom) {
  int status;
  size_t n, g;
  size_t iw, ih, ic;
  size_t ow, oh, oc;

  g  = this->group_;
  n  = this->num_;
  iw = this->width_;
  ih = this->height_;
  ic = this->channels_/g;

  CHECK(bottom[0]->width()    == iw &&
        bottom[0]->height()   == ih &&
        bottom[0]->channels() == ic*g &&
        bottom[0]->num()      == n)
          << "Incompatible shape of bottom with layer";

  ow = this->width_out_;
  oh = this->height_out_;
  oc = this->num_output_/g;
  CHECK(top[0]->width()    == ow &&
        top[0]->height()   == oh &&
        top[0]->channels() == oc*g &&
        top[0]->num()      == n) << "Incompatible shape of bottom with layer";

  if (propagate_down[0]) {
    void *res_convolutionBwdData[dnnResourceNumber];

    res_convolutionBwdData[dnnResourceDiffDst] =
      bwdd_top_diff->get_converted_prv(top[0], true);
    // Currently this conversion adds padding to weights.
    // We don't want that to be stored in the weights prv_ptr_
    res_convolutionBwdData[dnnResourceFilter]  =
      bwdd_filter_data->get_converted_prv(this->blobs_[0].get(), false);

    if (bwdd_bottom_diff->conversion_needed()) {
      bottom[0]->set_prv_diff_descriptor(bwdd_bottom_diff);
      res_convolutionBwdData[dnnResourceDiffSrc] =
              bottom[0]->mutable_prv_diff();
    } else {
      res_convolutionBwdData[dnnResourceDiffSrc] =
              bottom[0]->mutable_cpu_diff();
    }

    status = dnnExecute<Dtype>(convolutionBwdData, res_convolutionBwdData);
    CHECK_EQ(status, 0) << "Backward Data conv failed with status " << status;
  }

  if (this->param_propagate_down(0)) {
    void *res_convolutionBwdFilter[dnnResourceNumber];

    res_convolutionBwdFilter[dnnResourceDiffDst] =
            bwdf_top_diff->get_converted_prv(top[0], true);
    // The last get_converted_prv() argument is a hack for reusing conversion
    // done already in the forward direction.
    res_convolutionBwdFilter[dnnResourceSrc] =
            bwdf_bottom_data->get_converted_prv(bottom[0], false,
            fwd_bottom_data.get());

    if (bwdf_filter_diff->conversion_needed()) {
      this->blobs_[0]->set_prv_diff_descriptor(bwdf_filter_diff);
    }
    if (bwdf2fwd_filter_diff->conversion_needed()) {
      res_convolutionBwdFilter[dnnResourceDiffFilter] =
              reinterpret_cast<void *>(bwdf2fwd_filter_diff->prv_ptr());
    } else {
      if (bwdf_filter_diff->conversion_needed()) {
        res_convolutionBwdFilter[dnnResourceDiffFilter] =
                this->blobs_[0]->mutable_prv_diff();
      } else {
        res_convolutionBwdFilter[dnnResourceDiffFilter] =
                this->blobs_[0]->mutable_cpu_diff();
      }
    }

    status = dnnExecute<Dtype>(convolutionBwdFilter, res_convolutionBwdFilter);
    CHECK_EQ(status, 0) << "Backward Filter conv failed with status " << status;

    if (bwdf2fwd_filter_diff->conversion_needed()) {
      void *convert_resources[dnnResourceNumber];
      convert_resources[dnnResourceFrom] = bwdf2fwd_filter_diff->prv_ptr();
      if (bwdf_filter_diff->conversion_needed()) {
        convert_resources[dnnResourceTo] =
                this->blobs_[0]->mutable_prv_diff();
        DLOG(INFO) << "convert priv => priv  " << bwdf2fwd_filter_diff->name
           << " => " << bwdf_filter_diff->name;
      } else {
        convert_resources[dnnResourceTo] = this->blobs_[0]->mutable_cpu_diff();
        DLOG(INFO) << "convert priv =>       " << bwdf2fwd_filter_diff->name
           << " =>";
      }

      status = dnnExecute<Dtype>(bwdf2fwd_filter_diff->convert_from_int,
              convert_resources);
      CHECK_EQ(status, 0) << "Conversion failed with status " << status;
    }
  }

  if (this->param_propagate_down(1)) {
    void *res_convolutionBwdBias[dnnResourceNumber];

    res_convolutionBwdBias[dnnResourceDiffDst] =
            bwdb_top_diff->get_converted_prv(top[0], true);

    if (bwdb_bias_diff->conversion_needed()) {
      this->blobs_[1]->set_prv_diff_descriptor(bwdb_bias_diff);
      res_convolutionBwdBias[dnnResourceDiffBias] =
              reinterpret_cast<void *>(this->blobs_[1]->mutable_prv_diff());
    } else {
      res_convolutionBwdBias[dnnResourceDiffBias] =
              reinterpret_cast<void *>(this->blobs_[1]->mutable_cpu_diff());
    }
    status = dnnExecute<Dtype>(convolutionBwdBias, res_convolutionBwdBias);
    CHECK_EQ(status, 0) << "Backward Bias failed with status " << status;
  }
}

#ifdef CPU_ONLY
STUB_GPU(MKLConvolutionLayer);
#else
template <typename Dtype>
void MKLConvolutionLayer<Dtype>::Forward_gpu(
    const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top)
  {NOT_IMPLEMENTED;}
template <typename Dtype>
void MKLConvolutionLayer<Dtype>::Backward_gpu(
    const vector<Blob<Dtype>*>& top, const vector<bool>& propagate_down,
    const vector<Blob<Dtype>*>& bottom)
  {NOT_IMPLEMENTED;}
#endif

INSTANTIATE_CLASS(MKLConvolutionLayer);
}  // namespace caffe
#endif  // #ifdef MKL2017_SUPPORTED
