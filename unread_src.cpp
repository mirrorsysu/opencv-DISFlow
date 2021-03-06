
#include "precomp.hpp"
#include "opencv2/core/hal/intrin.hpp"
#include "opencl_kernels_video.hpp"

using namespace std;
#define EPS 0.001F
#define INF 1E+10F

namespace cv {

class DISOpticalFlowImpl CV_FINAL : public DISOpticalFlow
{
  public:
    DISOpticalFlowImpl();

    void calc(InputArray I0, InputArray I1, InputOutputArray flow) CV_OVERRIDE;
    void metal_calc(InputArray I0, InputArray I1, InputOutputArray flow, void *metal_PatchInverseSearch) CV_OVERRIDE;

    void collectGarbage() CV_OVERRIDE;

  protected: //!< algorithm parameters
    int finest_scale, coarsest_scale;
    int patch_size;
    int patch_stride;
    int grad_descent_iter;
    int variational_refinement_iter;
    float variational_refinement_alpha;
    float variational_refinement_gamma;
    float variational_refinement_delta;
    bool use_mean_normalization;
    bool use_spatial_propagation;

  protected: //!< some auxiliary variables
    int border_size;
    int w, h;   //!< flow buffer width and height on the current scale
    int ws, hs; //!< sparse flow buffer width and height on the current scale

  public:
    int getFinestScale() const CV_OVERRIDE { return finest_scale; }
    void setFinestScale(int val) CV_OVERRIDE { finest_scale = val; }
    int getPatchSize() const CV_OVERRIDE { return patch_size; }
    void setPatchSize(int val) CV_OVERRIDE { patch_size = val; }
    int getPatchStride() const CV_OVERRIDE { return patch_stride; }
    void setPatchStride(int val) CV_OVERRIDE { patch_stride = val; }
    int getGradientDescentIterations() const CV_OVERRIDE { return grad_descent_iter; }
    void setGradientDescentIterations(int val) CV_OVERRIDE { grad_descent_iter = val; }
    int getVariationalRefinementIterations() const CV_OVERRIDE { return variational_refinement_iter; }
    void setVariationalRefinementIterations(int val) CV_OVERRIDE { variational_refinement_iter = val; }
    float getVariationalRefinementAlpha() const CV_OVERRIDE { return variational_refinement_alpha; }
    void setVariationalRefinementAlpha(float val) CV_OVERRIDE { variational_refinement_alpha = val; }
    float getVariationalRefinementDelta() const CV_OVERRIDE { return variational_refinement_delta; }
    void setVariationalRefinementDelta(float val) CV_OVERRIDE { variational_refinement_delta = val; }
    float getVariationalRefinementGamma() const CV_OVERRIDE { return variational_refinement_gamma; }
    void setVariationalRefinementGamma(float val) CV_OVERRIDE { variational_refinement_gamma = val; }

    bool getUseMeanNormalization() const CV_OVERRIDE { return use_mean_normalization; }
    void setUseMeanNormalization(bool val) CV_OVERRIDE { use_mean_normalization = val; }
    bool getUseSpatialPropagation() const CV_OVERRIDE { return use_spatial_propagation; }
    void setUseSpatialPropagation(bool val) CV_OVERRIDE { use_spatial_propagation = val; }

  protected:                      //!< internal buffers
    vector<Mat_<uchar> > I0s;     //!< Gaussian pyramid for the current frame
    vector<Mat_<uchar> > I1s;     //!< Gaussian pyramid for the next frame
    vector<Mat_<uchar> > I1s_ext; //!< I1s with borders

    vector<Mat_<short> > I0xs; //!< Gaussian pyramid for the x gradient of the current frame
    vector<Mat_<short> > I0ys; //!< Gaussian pyramid for the y gradient of the current frame

    vector<Mat_<float> > Ux; //!< x component of the flow vectors
    vector<Mat_<float> > Uy; //!< y component of the flow vectors

    vector<Mat_<float> > initial_Ux; //!< x component of the initial flow field, if one was passed as an input
    vector<Mat_<float> > initial_Uy; //!< y component of the initial flow field, if one was passed as an input

    Mat_<Vec2f> U; //!< a buffer for the merged flow

    Mat_<float> Sx; //!< intermediate sparse flow representation (x component)
    Mat_<float> Sy; //!< intermediate sparse flow representation (y component)

    /* Structure tensor components: */
    Mat_<float> I0xx_buf; //!< sum of squares of x gradient values
    Mat_<float> I0yy_buf; //!< sum of squares of y gradient values
    Mat_<float> I0xy_buf; //!< sum of x and y gradient products

    /* Extra buffers that are useful if patch mean-normalization is used: */
    Mat_<float> I0x_buf; //!< sum of x gradient values
    Mat_<float> I0y_buf; //!< sum of y gradient values

    /* Auxiliary buffers used in structure tensor computation: */
    Mat_<float> I0xx_buf_aux;
    Mat_<float> I0yy_buf_aux;
    Mat_<float> I0xy_buf_aux;
    Mat_<float> I0x_buf_aux;
    Mat_<float> I0y_buf_aux;

    vector<Ptr<VariationalRefinement> > variational_refinement_processors;

  private: //!< private methods and parallel sections
    void prepareBuffers(Mat &I0, Mat &I1, Mat &flow, bool use_flow);
    void precomputeStructureTensor(Mat &dst_I0xx, Mat &dst_I0yy, Mat &dst_I0xy, Mat &dst_I0x, Mat &dst_I0y, Mat &I0x,
                                   Mat &I0y);
    int autoSelectCoarsestScale(int img_width);
    void autoSelectPatchSizeAndScales(int img_width);

    struct Densification_ParBody : public ParallelLoopBody
    {
        DISOpticalFlowImpl *dis;
        int nstripes, stripe_sz;
        int h;
        Mat *Ux, *Uy, *Sx, *Sy, *I0, *I1;

        Densification_ParBody(DISOpticalFlowImpl &_dis, int _nstripes, int _h, Mat &dst_Ux, Mat &dst_Uy, Mat &src_Sx,
                              Mat &src_Sy, Mat &_I0, Mat &_I1);
        void operator()(const Range &range) const CV_OVERRIDE;
    };

};

DISOpticalFlowImpl::DISOpticalFlowImpl()
{
    CV_INSTRUMENT_REGION();

    finest_scale = 2;
    patch_size = 8;
    patch_stride = 4;
    grad_descent_iter = 16;
    variational_refinement_iter = 5;
    variational_refinement_alpha = 20.f;
    variational_refinement_gamma = 10.f;
    variational_refinement_delta = 5.f;

    border_size = 16;
    use_mean_normalization = true;
    use_spatial_propagation = true;
    coarsest_scale = 10;

    /* Use separate variational refinement instances for different scales to avoid repeated memory allocation: */
    int max_possible_scales = 10;
    ws = hs = w = h = 0;
    for (int i = 0; i < max_possible_scales; i++)
        variational_refinement_processors.push_back(VariationalRefinement::create());
}

void DISOpticalFlowImpl::prepareBuffers(Mat &I0, Mat &I1, Mat &flow, bool use_flow)
{
    CV_INSTRUMENT_REGION();

    I0s.resize(coarsest_scale + 1);
    I1s.resize(coarsest_scale + 1);
    I1s_ext.resize(coarsest_scale + 1);
    I0xs.resize(coarsest_scale + 1);
    I0ys.resize(coarsest_scale + 1);
    Ux.resize(coarsest_scale + 1);
    Uy.resize(coarsest_scale + 1);

    Mat flow_uv[2];
    if (use_flow)
    {
        split(flow, flow_uv);
        initial_Ux.resize(coarsest_scale + 1);
        initial_Uy.resize(coarsest_scale + 1);
    }

    int fraction = 1;
    int cur_rows = 0, cur_cols = 0;

    for (int i = 0; i <= coarsest_scale; i++)
    {
        /* Avoid initializing the pyramid levels above the finest scale, as they won't be used anyway */
        if (i == finest_scale)
        {
            cur_rows = I0.rows / fraction;
            cur_cols = I0.cols / fraction;
            I0s[i].create(cur_rows, cur_cols);
            resize(I0, I0s[i], I0s[i].size(), 0.0, 0.0, INTER_AREA);
            I1s[i].create(cur_rows, cur_cols);
            resize(I1, I1s[i], I1s[i].size(), 0.0, 0.0, INTER_AREA);

            /* These buffers are reused in each scale so we initialize them once on the finest scale: */
            Sx.create(cur_rows / patch_stride, cur_cols / patch_stride);
            Sy.create(cur_rows / patch_stride, cur_cols / patch_stride);
            I0xx_buf.create(cur_rows / patch_stride, cur_cols / patch_stride);
            I0yy_buf.create(cur_rows / patch_stride, cur_cols / patch_stride);
            I0xy_buf.create(cur_rows / patch_stride, cur_cols / patch_stride);
            I0x_buf.create(cur_rows / patch_stride, cur_cols / patch_stride);
            I0y_buf.create(cur_rows / patch_stride, cur_cols / patch_stride);

            I0xx_buf_aux.create(cur_rows, cur_cols / patch_stride);
            I0yy_buf_aux.create(cur_rows, cur_cols / patch_stride);
            I0xy_buf_aux.create(cur_rows, cur_cols / patch_stride);
            I0x_buf_aux.create(cur_rows, cur_cols / patch_stride);
            I0y_buf_aux.create(cur_rows, cur_cols / patch_stride);

            U.create(cur_rows, cur_cols);
        }
        else if (i > finest_scale)
        {
            cur_rows = I0s[i - 1].rows / 2;
            cur_cols = I0s[i - 1].cols / 2;
            I0s[i].create(cur_rows, cur_cols);
            resize(I0s[i - 1], I0s[i], I0s[i].size(), 0.0, 0.0, INTER_AREA);
            I1s[i].create(cur_rows, cur_cols);
            resize(I1s[i - 1], I1s[i], I1s[i].size(), 0.0, 0.0, INTER_AREA);
        }

        if (i >= finest_scale)
        {
            I1s_ext[i].create(cur_rows + 2 * border_size, cur_cols + 2 * border_size);
            copyMakeBorder(I1s[i], I1s_ext[i], border_size, border_size, border_size, border_size, BORDER_REPLICATE);
            I0xs[i].create(cur_rows, cur_cols);
            I0ys[i].create(cur_rows, cur_cols);
            spatialGradient(I0s[i], I0xs[i], I0ys[i]);
            Ux[i].create(cur_rows, cur_cols);
            Uy[i].create(cur_rows, cur_cols);
            variational_refinement_processors[i]->setAlpha(variational_refinement_alpha);
            variational_refinement_processors[i]->setDelta(variational_refinement_delta);
            variational_refinement_processors[i]->setGamma(variational_refinement_gamma);
            variational_refinement_processors[i]->setSorIterations(5);
            variational_refinement_processors[i]->setFixedPointIterations(variational_refinement_iter);

            if (use_flow)
            {
                resize(flow_uv[0], initial_Ux[i], Size(cur_cols, cur_rows));
                initial_Ux[i] /= fraction;
                resize(flow_uv[1], initial_Uy[i], Size(cur_cols, cur_rows));
                initial_Uy[i] /= fraction;
            }
        }

        fraction *= 2;
    }
}


int DISOpticalFlowImpl::autoSelectCoarsestScale(int img_width)
{
    const int fratio = 5;
    return std::max(0, (int)std::floor(log2((2.0f*(float)img_width) / ((float)fratio * (float)patch_size))));
}

void DISOpticalFlowImpl::autoSelectPatchSizeAndScales(int img_width)
{
    switch (finest_scale)
    {
    case 1:
        patch_size = 8;
        coarsest_scale = autoSelectCoarsestScale(img_width);
        finest_scale = std::max(coarsest_scale-2, 0);
        break;

    case 3:
        patch_size = 12;
        coarsest_scale = autoSelectCoarsestScale(img_width);
        finest_scale = std::max(coarsest_scale-4, 0);
        break;

    case 4:
        patch_size = 12;
        coarsest_scale = autoSelectCoarsestScale(img_width);
        finest_scale = std::max(coarsest_scale-5, 0);
        break;

    // default case, fall-through.
    case 2:
    default:
        patch_size = 8;
        coarsest_scale = autoSelectCoarsestScale(img_width);
        finest_scale = std::max(coarsest_scale-2, 0);
        break;
    }
}


/////////////////////////////////////////////* Patch processing functions */////////////////////////////////////////////

/* Some auxiliary macros */
#define HAL_INIT_BILINEAR_8x8_PATCH_EXTRACTION                                                                         \
    v_float32x4 w00v = v_setall_f32(w00);                                                                              \
    v_float32x4 w01v = v_setall_f32(w01);                                                                              \
    v_float32x4 w10v = v_setall_f32(w10);                                                                              \
    v_float32x4 w11v = v_setall_f32(w11);                                                                              \
                                                                                                                       \
    v_uint16x8 I0_row_8, I1_row_8, I1_row_shifted_8, I1_row_next_8, I1_row_next_shifted_8, tmp;                        \
    v_uint32x4 I0_row_4_left, I1_row_4_left, I1_row_shifted_4_left, I1_row_next_4_left, I1_row_next_shifted_4_left;    \
    v_uint32x4 I0_row_4_right, I1_row_4_right, I1_row_shifted_4_right, I1_row_next_4_right,                            \
      I1_row_next_shifted_4_right;                                                                                     \
    v_float32x4 I_diff_left, I_diff_right;                                                                             \
                                                                                                                       \
    /* Preload and expand the first row of I1: */                                                                      \
    I1_row_8 = v_load_expand(I1_ptr);                                                                                  \
    I1_row_shifted_8 = v_load_expand(I1_ptr + 1);                                                                      \
    v_expand(I1_row_8, I1_row_4_left, I1_row_4_right);                                                                 \
    v_expand(I1_row_shifted_8, I1_row_shifted_4_left, I1_row_shifted_4_right);                                         \
    I1_ptr += I1_stride;

#define HAL_PROCESS_BILINEAR_8x8_PATCH_EXTRACTION                                                                      \
    /* Load the next row of I1: */                                                                                     \
    I1_row_next_8 = v_load_expand(I1_ptr);                                                                             \
    I1_row_next_shifted_8 = v_load_expand(I1_ptr + 1);                                                                 \
    /* Separate the left and right halves: */                                                                          \
    v_expand(I1_row_next_8, I1_row_next_4_left, I1_row_next_4_right);                                                  \
    v_expand(I1_row_next_shifted_8, I1_row_next_shifted_4_left, I1_row_next_shifted_4_right);                          \
                                                                                                                       \
    /* Load current row of I0: */                                                                                      \
    I0_row_8 = v_load_expand(I0_ptr);                                                                                  \
    v_expand(I0_row_8, I0_row_4_left, I0_row_4_right);                                                                 \
                                                                                                                       \
    /* Compute diffs between I0 and bilinearly interpolated I1: */                                                     \
    I_diff_left = w00v * v_cvt_f32(v_reinterpret_as_s32(I1_row_4_left)) +                                              \
                  w01v * v_cvt_f32(v_reinterpret_as_s32(I1_row_shifted_4_left)) +                                      \
                  w10v * v_cvt_f32(v_reinterpret_as_s32(I1_row_next_4_left)) +                                         \
                  w11v * v_cvt_f32(v_reinterpret_as_s32(I1_row_next_shifted_4_left)) -                                 \
                  v_cvt_f32(v_reinterpret_as_s32(I0_row_4_left));                                                      \
    I_diff_right = w00v * v_cvt_f32(v_reinterpret_as_s32(I1_row_4_right)) +                                            \
                   w01v * v_cvt_f32(v_reinterpret_as_s32(I1_row_shifted_4_right)) +                                    \
                   w10v * v_cvt_f32(v_reinterpret_as_s32(I1_row_next_4_right)) +                                       \
                   w11v * v_cvt_f32(v_reinterpret_as_s32(I1_row_next_shifted_4_right)) -                               \
                   v_cvt_f32(v_reinterpret_as_s32(I0_row_4_right));

#define HAL_BILINEAR_8x8_PATCH_EXTRACTION_NEXT_ROW                                                                     \
    I0_ptr += I0_stride;                                                                                               \
    I1_ptr += I1_stride;                                                                                               \
                                                                                                                       \
    I1_row_4_left = I1_row_next_4_left;                                                                                \
    I1_row_4_right = I1_row_next_4_right;                                                                              \
    I1_row_shifted_4_left = I1_row_next_shifted_4_left;                                                                \
    I1_row_shifted_4_right = I1_row_next_shifted_4_right;

/* This function essentially performs one iteration of gradient descent when finding the most similar patch in I1 for a
 * given one in I0. It assumes that I0_ptr and I1_ptr already point to the corresponding patches and w00, w01, w10, w11
 * are precomputed bilinear interpolation weights. It returns the SSD (sum of squared differences) between these patches
 * and computes the values (dst_dUx, dst_dUy) that are used in the flow vector update. HAL acceleration is implemented
 * only for the default patch size (8x8). Everything is processed in floats as using fixed-point approximations harms
 * the quality significantly.
 */
inline float processPatch(float &dst_dUx, float &dst_dUy, uchar *I0_ptr, uchar *I1_ptr, short *I0x_ptr, short *I0y_ptr,
                          int I0_stride, int I1_stride, float w00, float w01, float w10, float w11, int patch_sz)
{
    float SSD = 0.0f;
#if CV_SIMD128
    if (patch_sz == 8)
    {
        /* Variables to accumulate the sums */
        v_float32x4 Ux_vec = v_setall_f32(0);
        v_float32x4 Uy_vec = v_setall_f32(0);
        v_float32x4 SSD_vec = v_setall_f32(0);

        v_int16x8 I0x_row, I0y_row;
        v_int32x4 I0x_row_4_left, I0x_row_4_right, I0y_row_4_left, I0y_row_4_right;

        HAL_INIT_BILINEAR_8x8_PATCH_EXTRACTION;
        for (int row = 0; row < 8; row++)
        {
            HAL_PROCESS_BILINEAR_8x8_PATCH_EXTRACTION;
            I0x_row = v_load(I0x_ptr);
            v_expand(I0x_row, I0x_row_4_left, I0x_row_4_right);
            I0y_row = v_load(I0y_ptr);
            v_expand(I0y_row, I0y_row_4_left, I0y_row_4_right);

            /* Update the sums: */
            Ux_vec += I_diff_left * v_cvt_f32(I0x_row_4_left) + I_diff_right * v_cvt_f32(I0x_row_4_right);
            Uy_vec += I_diff_left * v_cvt_f32(I0y_row_4_left) + I_diff_right * v_cvt_f32(I0y_row_4_right);
            SSD_vec += I_diff_left * I_diff_left + I_diff_right * I_diff_right;

            I0x_ptr += I0_stride;
            I0y_ptr += I0_stride;
            HAL_BILINEAR_8x8_PATCH_EXTRACTION_NEXT_ROW;
        }

        /* Final reduce operations: */
        dst_dUx = v_reduce_sum(Ux_vec);
        dst_dUy = v_reduce_sum(Uy_vec);
        SSD = v_reduce_sum(SSD_vec);
    }
    else
#endif
    {
        dst_dUx = 0.0f;
        dst_dUy = 0.0f;
        float diff;
        for (int i = 0; i < patch_sz; i++)
            for (int j = 0; j < patch_sz; j++)
            {
                diff = w00 * I1_ptr[i * I1_stride + j] + w01 * I1_ptr[i * I1_stride + j + 1] +
                       w10 * I1_ptr[(i + 1) * I1_stride + j] + w11 * I1_ptr[(i + 1) * I1_stride + j + 1] -
                       I0_ptr[i * I0_stride + j];

                SSD += diff * diff;
                dst_dUx += diff * I0x_ptr[i * I0_stride + j];
                dst_dUy += diff * I0y_ptr[i * I0_stride + j];
            }
    }
    return SSD;
}

/* Similar to processPatch, but compute only the sum of squared differences (SSD) between the patches */
inline float computeSSD(uchar *I0_ptr, uchar *I1_ptr, int I0_stride, int I1_stride, float w00, float w01, float w10,
                        float w11, int patch_sz)
{
    float SSD = 0.0f;
#if CV_SIMD128
    if (patch_sz == 8)
    {
        v_float32x4 SSD_vec = v_setall_f32(0);
        HAL_INIT_BILINEAR_8x8_PATCH_EXTRACTION;
        for (int row = 0; row < 8; row++)
        {
            HAL_PROCESS_BILINEAR_8x8_PATCH_EXTRACTION;
            SSD_vec += I_diff_left * I_diff_left + I_diff_right * I_diff_right;
            HAL_BILINEAR_8x8_PATCH_EXTRACTION_NEXT_ROW;
        }
        SSD = v_reduce_sum(SSD_vec);
    }
    else
#endif
    {
        float diff;
        for (int i = 0; i < patch_sz; i++)
            for (int j = 0; j < patch_sz; j++)
            {
                diff = w00 * I1_ptr[i * I1_stride + j] + w01 * I1_ptr[i * I1_stride + j + 1] +
                       w10 * I1_ptr[(i + 1) * I1_stride + j] + w11 * I1_ptr[(i + 1) * I1_stride + j + 1] -
                       I0_ptr[i * I0_stride + j];
                SSD += diff * diff;
            }
    }
    return SSD;
}

/* Same as computeSSD, but with patch mean normalization */
inline float computeSSDMeanNorm(uchar *I0_ptr, uchar *I1_ptr, int I0_stride, int I1_stride, float w00, float w01,
                                float w10, float w11, int patch_sz)
{
    float sum_diff = 0.0f, sum_diff_sq = 0.0f;
    float n = (float)patch_sz * patch_sz;
#if CV_SIMD128
    if (patch_sz == 8)
    {
        v_float32x4 sum_diff_vec = v_setall_f32(0);
        v_float32x4 sum_diff_sq_vec = v_setall_f32(0);
        HAL_INIT_BILINEAR_8x8_PATCH_EXTRACTION;
        for (int row = 0; row < 8; row++)
        {
            HAL_PROCESS_BILINEAR_8x8_PATCH_EXTRACTION;
            sum_diff_sq_vec += I_diff_left * I_diff_left + I_diff_right * I_diff_right;
            sum_diff_vec += I_diff_left + I_diff_right;
            HAL_BILINEAR_8x8_PATCH_EXTRACTION_NEXT_ROW;
        }
        sum_diff = v_reduce_sum(sum_diff_vec);
        sum_diff_sq = v_reduce_sum(sum_diff_sq_vec);
    }
    else
    {
#endif
        float diff;
        for (int i = 0; i < patch_sz; i++)
            for (int j = 0; j < patch_sz; j++)
            {
                diff = w00 * I1_ptr[i * I1_stride + j] + w01 * I1_ptr[i * I1_stride + j + 1] +
                       w10 * I1_ptr[(i + 1) * I1_stride + j] + w11 * I1_ptr[(i + 1) * I1_stride + j + 1] -
                       I0_ptr[i * I0_stride + j];

                sum_diff += diff;
                sum_diff_sq += diff * diff;
            }
#if CV_SIMD128
    }
#endif
    return sum_diff_sq - sum_diff * sum_diff / n;
}

#undef HAL_INIT_BILINEAR_8x8_PATCH_EXTRACTION
#undef HAL_PROCESS_BILINEAR_8x8_PATCH_EXTRACTION
#undef HAL_BILINEAR_8x8_PATCH_EXTRACTION_NEXT_ROW
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

DISOpticalFlowImpl::Densification_ParBody::Densification_ParBody(DISOpticalFlowImpl &_dis, int _nstripes, int _h,
                                                                 Mat &dst_Ux, Mat &dst_Uy, Mat &src_Sx, Mat &src_Sy,
                                                                 Mat &_I0, Mat &_I1)
    : dis(&_dis), nstripes(_nstripes), h(_h), Ux(&dst_Ux), Uy(&dst_Uy), Sx(&src_Sx), Sy(&src_Sy), I0(&_I0), I1(&_I1)
{
    stripe_sz = (int)ceil(h / (double)nstripes);
}

/* This function transforms a sparse optical flow field obtained by PatchInverseSearch (which computes flow values
 * on a sparse grid defined by patch_stride) into a dense optical flow field by weighted averaging of values from the
 * overlapping patches.
 */
void DISOpticalFlowImpl::Densification_ParBody::operator()(const Range &range) const
{
    CV_INSTRUMENT_REGION();

    int start_i = min(range.start * stripe_sz, h);
    int end_i = min(range.end * stripe_sz, h);

    /* Input sparse flow */
    float *Sx_ptr = Sx->ptr<float>();
    float *Sy_ptr = Sy->ptr<float>();

    /* Output dense flow */
    float *Ux_ptr = Ux->ptr<float>();
    float *Uy_ptr = Uy->ptr<float>();

    uchar *I0_ptr = I0->ptr<uchar>();
    uchar *I1_ptr = I1->ptr<uchar>();

    int psz = dis->patch_size;
    int pstr = dis->patch_stride;
    int i_l, i_u;
    int j_l, j_u;
    float i_m, j_m, diff;

    /* These values define the set of sparse grid locations that contain patches overlapping with the current dense flow
     * location */
    int start_is, end_is;
    int start_js, end_js;

/* Some helper macros for updating this set of sparse grid locations */
#define UPDATE_SPARSE_I_COORDINATES                                                                                    \
    if (i % pstr == 0 && i + psz <= h)                                                                                 \
        end_is++;                                                                                                      \
    if (i - psz >= 0 && (i - psz) % pstr == 0 && start_is < end_is)                                                    \
        start_is++;

#define UPDATE_SPARSE_J_COORDINATES                                                                                    \
    if (j % pstr == 0 && j + psz <= dis->w)                                                                            \
        end_js++;                                                                                                      \
    if (j - psz >= 0 && (j - psz) % pstr == 0 && start_js < end_js)                                                    \
        start_js++;

    start_is = 0;
    end_is = -1;
    for (int i = 0; i < start_i; i++)
    {
        UPDATE_SPARSE_I_COORDINATES;
    }
    for (int i = start_i; i < end_i; i++)
    {
        UPDATE_SPARSE_I_COORDINATES;
        start_js = 0;
        end_js = -1;
        for (int j = 0; j < dis->w; j++)
        {
            UPDATE_SPARSE_J_COORDINATES;
            float coef, sum_coef = 0.0f;
            float sum_Ux = 0.0f;
            float sum_Uy = 0.0f;

            /* Iterate through all the patches that overlap the current location (i,j) */
            for (int is = start_is; is <= end_is; is++)
                for (int js = start_js; js <= end_js; js++)
                {
                    j_m = min(max(j + Sx_ptr[is * dis->ws + js], 0.0f), dis->w - 1.0f - EPS);
                    i_m = min(max(i + Sy_ptr[is * dis->ws + js], 0.0f), dis->h - 1.0f - EPS);
                    j_l = (int)j_m;
                    j_u = j_l + 1;
                    i_l = (int)i_m;
                    i_u = i_l + 1;
                    diff = (j_m - j_l) * (i_m - i_l) * I1_ptr[i_u * dis->w + j_u] +
                           (j_u - j_m) * (i_m - i_l) * I1_ptr[i_u * dis->w + j_l] +
                           (j_m - j_l) * (i_u - i_m) * I1_ptr[i_l * dis->w + j_u] +
                           (j_u - j_m) * (i_u - i_m) * I1_ptr[i_l * dis->w + j_l] - I0_ptr[i * dis->w + j];
                    coef = 1 / max(1.0f, abs(diff));
                    sum_Ux += coef * Sx_ptr[is * dis->ws + js];
                    sum_Uy += coef * Sy_ptr[is * dis->ws + js];
                    sum_coef += coef;
                }
            CV_DbgAssert(sum_coef != 0);
            Ux_ptr[i * dis->w + j] = sum_Ux / sum_coef;
            Uy_ptr[i * dis->w + j] = sum_Uy / sum_coef;
        }
    }
#undef UPDATE_SPARSE_I_COORDINATES
#undef UPDATE_SPARSE_J_COORDINATES
}

void DISOpticalFlowImpl::calc(InputArray I0, InputArray I1, InputOutputArray flow)
{
    CV_INSTRUMENT_REGION();

    CV_Assert(!I0.empty() && I0.depth() == CV_8U && I0.channels() == 1);
    CV_Assert(!I1.empty() && I1.depth() == CV_8U && I1.channels() == 1);
    CV_Assert(I0.sameSize(I1));
    CV_Assert(I0.isContinuous());
    CV_Assert(I1.isContinuous());

    Mat I0Mat = I0.getMat();
    Mat I1Mat = I1.getMat();
    bool use_input_flow = false;
    if (flow.sameSize(I0) && flow.depth() == CV_32F && flow.channels() == 2)
        use_input_flow = true;
    else
        flow.create(I1Mat.size(), CV_32FC2);
    Mat flowMat = flow.getMat();
    coarsest_scale = min((int)(log(max(I0Mat.cols, I0Mat.rows) / (4.0 * patch_size)) / log(2.0) + 0.5), /* Original code search for maximal movement of width/4 */
                         (int)(log(min(I0Mat.cols, I0Mat.rows) / patch_size) / log(2.0)));              /* Deepest pyramid level greater or equal than patch*/

    if (coarsest_scale<0)
        CV_Error(cv::Error::StsBadSize, "The input image must have either width or height >= 12");

    if (coarsest_scale<finest_scale)
    {
        // choose the finest level based on coarsest level.
        // Refs: https://github.com/tikroeger/OF_DIS/blob/2c9f2a674f3128d3a41c10e41cc9f3a35bb1b523/run_dense.cpp#L239
        int original_img_width = I0.size().width;
        autoSelectPatchSizeAndScales(original_img_width);
    }

    int num_stripes = getNumThreads();

    prepareBuffers(I0Mat, I1Mat, flowMat, use_input_flow);
    Ux[coarsest_scale].setTo(0.0f);
    Uy[coarsest_scale].setTo(0.0f);

    for (int i = coarsest_scale; i >= finest_scale; i--)
    {
        CV_TRACE_REGION("coarsest_scale_iteration");
        w = I0s[i].cols;
        h = I0s[i].rows;
        ws = 1 + (w - patch_size) / patch_stride;
        hs = 1 + (h - patch_size) / patch_stride;

        precomputeStructureTensor(I0xx_buf, I0yy_buf, I0xy_buf, I0x_buf, I0y_buf, I0xs[i], I0ys[i]);
        if (use_spatial_propagation)
        {
            /* Use a fixed number of stripes regardless the number of threads to make inverse search
             * with spatial propagation reproducible
             */
            parallel_for_(Range(0, 8), PatchInverseSearch_ParBody(*this, 8, hs, Sx, Sy, Ux[i], Uy[i], I0s[i],
                                                                  I1s_ext[i], I0xs[i], I0ys[i], 2, i));
        }
        else
        {
            parallel_for_(Range(0, num_stripes),
                          PatchInverseSearch_ParBody(*this, num_stripes, hs, Sx, Sy, Ux[i], Uy[i], I0s[i], I1s_ext[i],
                                                     I0xs[i], I0ys[i], 1, i));
        }

        parallel_for_(Range(0, num_stripes),
                      Densification_ParBody(*this, num_stripes, I0s[i].rows, Ux[i], Uy[i], Sx, Sy, I0s[i], I1s[i]));
        if (variational_refinement_iter > 0)
            variational_refinement_processors[i]->calcUV(I0s[i], I1s[i], Ux[i], Uy[i]);

        if (i > finest_scale)
        {
            resize(Ux[i], Ux[i - 1], Ux[i - 1].size());
            resize(Uy[i], Uy[i - 1], Uy[i - 1].size());
            Ux[i - 1] *= 2;
            Uy[i - 1] *= 2;
        }
    }
    Mat uxy[] = {Ux[finest_scale], Uy[finest_scale]};
    merge(uxy, 2, U);
    resize(U, flowMat, flowMat.size());
    flowMat *= 1 << finest_scale;
}

void DISOpticalFlowImpl::collectGarbage()
{
    CV_INSTRUMENT_REGION();

    I0s.clear();
    I1s.clear();
    I1s_ext.clear();
    I0xs.clear();
    I0ys.clear();
    Ux.clear();
    Uy.clear();
    U.release();
    Sx.release();
    Sy.release();
    I0xx_buf.release();
    I0yy_buf.release();
    I0xy_buf.release();
    I0xx_buf_aux.release();
    I0yy_buf_aux.release();
    I0xy_buf_aux.release();

    for (int i = finest_scale; i <= coarsest_scale; i++)
        variational_refinement_processors[i]->collectGarbage();
    variational_refinement_processors.clear();
}

Ptr<DISOpticalFlow> DISOpticalFlow::create(int preset)
{
    CV_INSTRUMENT_REGION();

    Ptr<DISOpticalFlow> dis = makePtr<DISOpticalFlowImpl>();
    dis->setPatchSize(8);
    if (preset == DISOpticalFlow::PRESET_ULTRAFAST)
    {
        dis->setFinestScale(2);
        dis->setPatchStride(4);
        dis->setGradientDescentIterations(12);
        dis->setVariationalRefinementIterations(0);
    }
    else if (preset == DISOpticalFlow::PRESET_FAST)
    {
        dis->setFinestScale(2);
        dis->setPatchStride(4);
        dis->setGradientDescentIterations(16);
        dis->setVariationalRefinementIterations(5);
    }
    else if (preset == DISOpticalFlow::PRESET_MEDIUM)
    {
        dis->setFinestScale(1);
        dis->setPatchStride(3);
        dis->setGradientDescentIterations(25);
        dis->setVariationalRefinementIterations(5);
    }

    return dis;
}


} // namespace
