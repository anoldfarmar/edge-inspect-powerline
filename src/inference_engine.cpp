#include "edgeinspect/inference_engine.h"
#include "insplad/scoring.h"
#include "rknn_api.h"
#include <omp.h>
#include <dlfcn.h>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace edgeinspect {
namespace {
void check(int ret,const char* op) {
    if(ret!=RKNN_SUCC) throw std::runtime_error(std::string(op)+" ret="+std::to_string(ret));
}
// Declared first in Impl: destroyed last, also during a failed constructor.
struct Context {
    rknn_context value=0;
    ~Context() noexcept { if(value) { const int r=rknn_destroy(value); if(r!=RKNN_SUCC) std::cerr<<"rknn_destroy ret="<<r<<'\n'; } }
};
struct OutputLease {
    rknn_context context;
    std::vector<rknn_output> values;
    bool acquired=false;
    explicit OutputLease(rknn_context ctx):context(ctx),values(2) {
        for(unsigned i=0;i<2;++i) { values[i].index=i; values[i].want_float=1; }
        check(rknn_outputs_get(context,2,values.data(),nullptr),"outputs_get"); acquired=true;
    }
    void release() {
        if(acquired) { acquired=false; check(rknn_outputs_release(context,2,values.data()),"outputs_release"); }
    }
    ~OutputLease() noexcept {
        if(acquired) { const int r=rknn_outputs_release(context,2,values.data()); if(r!=RKNN_SUCC) std::cerr<<"outputs_release ret="<<r<<'\n'; }
    }
    OutputLease(const OutputLease&)=delete;
    OutputLease& operator=(const OutputLease&)=delete;
};
std::string attribute(const rknn_tensor_attr& a) {
    std::ostringstream s; s<<"{\"index\":"<<a.index<<",\"name\":"<<json_string(a.name)<<",\"dims\":[";
    for(unsigned i=0;i<a.n_dims;++i) s<<(i?",":"")<<a.dims[i];
    s<<"],\"fmt\":"<<json_string(get_format_string(a.fmt))<<",\"type\":"<<json_string(get_type_string(a.type))
     <<",\"qnt_type\":"<<json_string(get_qnt_type_string(a.qnt_type))<<",\"scale\":"<<a.scale
     <<",\"zp\":"<<a.zp<<",\"n_elems\":"<<a.n_elems<<",\"size\":"<<a.size
     <<",\"w_stride\":"<<a.w_stride<<",\"h_stride\":"<<a.h_stride<<",\"size_with_stride\":"<<a.size_with_stride<<'}';
    return s.str();
}
const std::map<std::string,int> classes={{"glass-insulator",7},{"lightning-rod-suspension",5},
    {"polymer-insulator-upper-shackle",11},{"vari-grip",9},{"yoke-suspension",1}};
struct Bank { std::vector<float> data; std::string sha; };
Bank load_bank(const std::filesystem::path& path) {
    if(std::filesystem::file_size(path)!=409600) throw std::runtime_error("Bank must be FP32 [1600,64]: "+path.string());
    Bank b; b.data.resize(102400); std::ifstream f(path,std::ios::binary);
    if(!f.read(reinterpret_cast<char*>(b.data.data()),409600)) throw std::runtime_error("Bank read failed: "+path.string());
    for(float x:b.data) if(!std::isfinite(x)) throw std::runtime_error("Nonfinite bank");
    b.sha=sha256_bytes(b.data.data(),409600); return b;
}
}
int asset_class(const std::string& asset) {
    const auto it=classes.find(asset);
    if(it==classes.end()) throw std::invalid_argument("NO_BANK: unknown asset "+asset);
    return it->second;
}
struct InferenceEngine::Impl {
    Context context;
    std::vector<rknn_tensor_attr> outputs;
    std::map<std::string,Bank> banks;
    unsigned predictions_index=0,p3_index=0;
    std::string environment;
    explicit Impl(const Config& config) {
        const auto model=config.root/"models/insplad_shared_p3_320_fp.rknn";
        if(sha256_file(model)!=kModelSha256) throw std::runtime_error("Model SHA256 mismatch");
        const auto path=model.string();
        check(rknn_init(&context.value,const_cast<char*>(path.c_str()),0,0,nullptr),"rknn_init");
        rknn_sdk_version version{}; rknn_input_output_num io{};
        check(rknn_query(context.value,RKNN_QUERY_SDK_VERSION,&version,sizeof(version)),"SDK_VERSION");
        check(rknn_query(context.value,RKNN_QUERY_IN_OUT_NUM,&io,sizeof(io)),"IN_OUT_NUM");
        if(io.n_input!=1 || io.n_output!=2) throw std::runtime_error("Unexpected input/output count");
        Dl_info lib{}; dladdr(reinterpret_cast<void*>(&rknn_query),&lib);
        std::ostringstream env;
        env<<"{\"model_sha256\":"<<json_string(kModelSha256)<<",\"runtime\":"<<json_string(version.api_version)
           <<",\"driver\":"<<json_string(version.drv_version)<<",\"runtime_library\":"<<json_string(lib.dli_fname?lib.dli_fname:"")
           <<",\"npu_core\":\"AUTO\",\"scoring_threads\":4";
        const char* keys[]={"inputs","outputs","native_inputs","native_outputs"};
        const rknn_query_cmd commands[]={RKNN_QUERY_INPUT_ATTR,RKNN_QUERY_OUTPUT_ATTR,RKNN_QUERY_NATIVE_INPUT_ATTR,RKNN_QUERY_NATIVE_OUTPUT_ATTR};
        bool pred=false,p3=false;
        for(unsigned group=0;group<4;++group) {
            env<<",\""<<keys[group]<<"\":[";
            const unsigned count=group%2?2:1;
            for(unsigned i=0;i<count;++i) {
                rknn_tensor_attr a{}; a.index=i;
                check(rknn_query(context.value,commands[group],&a,sizeof(a)),keys[group]);
                if(a.n_dims>RKNN_MAX_DIMS) throw std::runtime_error("Invalid tensor rank");
                if(i) env<<',';
                env<<attribute(a);
                if(group==0 && (std::string(a.name)!="images" || a.n_dims!=4 || a.fmt!=RKNN_TENSOR_NHWC ||
                    a.dims[0]!=1 || a.dims[1]!=320 || a.dims[2]!=320 || a.dims[3]!=3 || a.n_elems!=307200))
                    throw std::runtime_error("Unexpected input contract");
                if(group==1) {
                    if(std::string(a.name)=="predictions" && a.n_dims==3 && a.dims[0]==1 && a.dims[1]==18 && a.dims[2]==2100 &&
                        a.n_elems==37800 && (a.fmt==RKNN_TENSOR_UNDEFINED || a.fmt==RKNN_TENSOR_NCHW)) {pred=true;predictions_index=i;}
                    else if(std::string(a.name)=="backbone_p3" && a.n_dims==4 && a.dims[0]==1 && a.dims[1]==64 && a.dims[2]==40 && a.dims[3]==40 &&
                        a.n_elems==102400 && a.fmt==RKNN_TENSOR_NCHW) {p3=true;p3_index=i;}
                    else throw std::runtime_error("Unexpected output name/shape/layout");
                    outputs.push_back(a);
                }
            }
            env<<']';
        }
        if(!pred || !p3) throw std::runtime_error("Missing named outputs");
        check(rknn_set_core_mask(context.value,RKNN_NPU_CORE_AUTO),"core_mask AUTO");
        for(const auto& [asset,unused]:classes) {
            (void)unused; banks.emplace(asset,load_bank(config.root/"banks/matched_rknn"/(asset+".f32")));
        }
        env<<'}'; environment=env.str();
    }
};
InferenceEngine::InferenceEngine(const Config& c):impl_(std::make_unique<Impl>(c)) {}
InferenceEngine::~InferenceEngine()=default;
const std::string& InferenceEngine::environment_json() const {return impl_->environment;}
Result InferenceEngine::infer(const PreparedInput& input,const std::string& asset,bool diagnostics) {
    using Clock=std::chrono::steady_clock;
    if(input.rgb.size()!=307200) throw std::invalid_argument("RGB input size mismatch");
    const int cls=asset_class(asset); const auto& bank=impl_->banks.at(asset);
    omp_set_dynamic(0); omp_set_num_threads(4);
    const auto start=Clock::now();
    rknn_input in{}; in.index=0; in.buf=const_cast<std::uint8_t*>(input.rgb.data()); in.size=input.rgb.size();
    in.type=RKNN_TENSOR_UINT8; in.fmt=RKNN_TENSOR_NHWC; in.pass_through=0;
    check(rknn_inputs_set(impl_->context.value,1,&in),"inputs_set");
    check(rknn_run(impl_->context.value,nullptr),"run");
    OutputLease lease(impl_->context.value);
    for(unsigned i=0;i<2;++i) if(lease.values[i].size!=impl_->outputs[i].n_elems*sizeof(float) || !lease.values[i].buf)
        throw std::runtime_error("FP32 logical output size mismatch");
    const auto model_done=Clock::now();
    const auto* predictions=static_cast<const float*>(lease.values[impl_->predictions_index].buf);
    const auto* p3=static_cast<const float*>(lease.values[impl_->p3_index].buf);
    for(std::size_t i=0;i<37800;++i) if(!std::isfinite(predictions[i])) throw std::runtime_error("Nonfinite predictions");
    const auto detections=insplad::decode_nms(predictions,37800);
    Result result; result.asset=asset; result.bank_sha256=bank.sha;
    result.selection=insplad::select_asset(detections,cls);
    auto post=insplad::postprocess_p3(p3,102400,result.selection.box);
    const auto roi_done=Clock::now();
    const auto distances=insplad::nearest_euclidean(post.normalized_patches.data(),1600,bank.data.data(),1600,256);
    result.score=insplad::top8_score(distances);
    const auto score_done=Clock::now();
    if(diagnostics) {
        result.diagnostics.emplace(); auto& d=*result.diagnostics;
        d.predictions.assign(predictions,predictions+37800); d.p3.assign(p3,p3+102400);
        d.roi=std::move(post.roi); d.normalized_patches=std::move(post.normalized_patches);
    }
    lease.release();
    auto ms=[](auto a,auto b){return std::chrono::duration<double,std::milli>(b-a).count();};
    result.model_ms=ms(start,model_done); result.detection_roi_ms=ms(model_done,roi_done);
    result.retrieval_ms=ms(roi_done,score_done); result.inference_total_ms=ms(start,Clock::now());
    return result;
}
} // namespace edgeinspect
