#include "rknn_api.h"
#include "insplad/detection.h"
#include "insplad/scoring.h"
#include <omp.h>
#include <sys/resource.h>
#include <map>
#include <memory>
#include <sstream>
#include <algorithm>
#include <cmath>
// Ordinary RKNN API adapted from the previously successful board_probe.cpp.
// See tests/run_rknn_pipeline.py provenance for the original source hash.
#include <dlfcn.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <vector>

static void check(int ret, const char* op) {
    if (ret != RKNN_SUCC) throw std::runtime_error(std::string(op) + " ret=" + std::to_string(ret));
}
static void attr_json(std::ostream& f, const rknn_tensor_attr& a) {
    f << "{\"index\":" << a.index << ",\"name\":" << std::quoted(a.name)
      << ",\"n_dims\":" << a.n_dims << ",\"dims\":[";
    for (unsigned j=0; j<a.n_dims; ++j) f << (j ? "," : "") << a.dims[j];
    f << "],\"fmt\":" << std::quoted(get_format_string(a.fmt))
      << ",\"type\":" << std::quoted(get_type_string(a.type))
      << ",\"qnt_type\":" << std::quoted(get_qnt_type_string(a.qnt_type))
      << ",\"scale\":" << a.scale << ",\"zp\":" << a.zp << ",\"fl\":" << int(a.fl)
      << ",\"size\":" << a.size << ",\"n_elems\":" << a.n_elems
      << ",\"w_stride\":" << a.w_stride << ",\"h_stride\":" << a.h_stride
      << ",\"size_with_stride\":" << a.size_with_stride << "}";
}
// Shared ordinary-API initialization/query contract from rknn_pipeline.cpp.
struct Job { std::string id,rgb,asset;int cls;std::vector<unsigned char> bytes; };
static std::vector<unsigned char> load_rgb(const std::string& path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f || f.tellg()!=307200) throw std::runtime_error("Bad RGB bytes: "+path);
    f.seekg(0);std::vector<unsigned char> v(307200);
    if(!f.read(reinterpret_cast<char*>(v.data()),v.size())) throw std::runtime_error("RGB read failed");
    return v;
}
static std::vector<float> load_bank(const std::string& path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f || f.tellg()!=409600) throw std::runtime_error("Bank must be [1600,64]: "+path);
    f.seekg(0);std::vector<float> v(102400);
    if(!f.read(reinterpret_cast<char*>(v.data()),v.size()*4)) throw std::runtime_error("Bank read failed");
    return v;
}
static double temperature() {
    double maximum=-1;
    for(int i=0;i<16;++i) { std::ifstream f("/sys/class/thermal/thermal_zone"+std::to_string(i)+"/temp");double t;if(f>>t) maximum=std::max(maximum,t/1000); }
    return maximum;
}
int main(int argc,char** argv) {
    if(argc!=8) { std::cerr << "Usage: rknn_eval MODEL BANK_DIR JOBS.tsv OUT.jsonl ROUNDS WARMUP THREADS\n";return 1; }
    rknn_context ctx=0;std::vector<rknn_output> outputs;bool acquired=false;
    try {
        const int rounds=std::stoi(argv[5]),warmup=std::stoi(argv[6]),threads=std::stoi(argv[7]);
        if(rounds<1 || warmup<0 || threads<1) throw std::runtime_error("Invalid timing parameters");
        omp_set_dynamic(0);omp_set_num_threads(threads);
        // Attribute file goes beside output journal. No per-image tensor dumps.
        std::string output_path=argv[4];
        check(rknn_init(&ctx, argv[1], 0, 0, nullptr), "rknn_init");
        rknn_sdk_version ver{};
        check(rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver)), "SDK_VERSION");
        rknn_input_output_num io{};
        check(rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)), "IN_OUT_NUM");
        std::ofstream attrs(output_path+".io_attributes.json");
        attrs.exceptions(std::ios::badbit | std::ios::failbit);
        Dl_info lib{}; dladdr(reinterpret_cast<void*>(&rknn_query), &lib);
        attrs << "{\"runtime\":" << std::quoted(ver.api_version) << ",\"driver\":" << std::quoted(ver.drv_version)
              << ",\"library\":" << std::quoted(lib.dli_fname ? lib.dli_fname : "unknown");
        std::cout << "Runtime: " << ver.api_version << " Driver: " << ver.drv_version << std::endl;
        std::vector<rknn_tensor_attr> logical_in, logical_out;
        const char* keys[]={"inputs", "outputs", "native_inputs", "native_outputs"};
        rknn_query_cmd cmds[]={RKNN_QUERY_INPUT_ATTR, RKNN_QUERY_OUTPUT_ATTR,
                              RKNN_QUERY_NATIVE_INPUT_ATTR, RKNN_QUERY_NATIVE_OUTPUT_ATTR};
        for (int group=0; group<4; ++group) {
            attrs << ",\"" << keys[group] << "\":[";
            unsigned count=(group%2==0) ? io.n_input : io.n_output;
            for (unsigned i=0; i<count; ++i) {
                rknn_tensor_attr a{}; a.index=i;
                check(rknn_query(ctx, cmds[group], &a, sizeof(a)), keys[group]);
                if (a.n_dims>RKNN_MAX_DIMS) throw std::runtime_error("Invalid n_dims");
                if (i) attrs << ',';
                attr_json(attrs, a);
                std::cout << keys[group] << ' '; attr_json(std::cout, a); std::cout << std::endl;
                if (group==0) logical_in.push_back(a);
                if (group==1) logical_out.push_back(a);
            }
            attrs << ']';
        }
        attrs << "}\n"; attrs.close();
        {
            if (logical_in.size()!=1 || std::string(logical_in[0].name)!="images" ||
                logical_in[0].n_elems!=320*320*3 || logical_in[0].n_dims!=4 ||
                logical_in[0].fmt!=RKNN_TENSOR_NHWC || logical_in[0].dims[0]!=1 ||
                logical_in[0].dims[1]!=320 || logical_in[0].dims[2]!=320 || logical_in[0].dims[3]!=3)
                throw std::runtime_error("Unexpected input contract");
            // Reject unknown output roles/layouts before interpreting or running fixtures.
            if (logical_out.size()!=2) throw std::runtime_error("Expected two outputs");
            bool pred=false, p3=false;
            for (const auto& a: logical_out) {
                std::string name(a.name);
                // Rank-3 BCN has no image-layout tag; RKNN reports UNDEFINED.
                // Ordinary outputs_get returns logical contiguous elements;
                // fixed-fixture comparison independently checks the BCN ordering.
                if (name=="predictions" && a.n_dims==3 && a.dims[0]==1 && a.dims[1]==18 && a.dims[2]==2100 && (a.fmt==RKNN_TENSOR_UNDEFINED || a.fmt==RKNN_TENSOR_NCHW)) pred=true;
                else if (name=="backbone_p3" && a.n_dims==4 && a.dims[0]==1 && a.dims[1]==64 && a.dims[2]==40 && a.dims[3]==40 && a.fmt==RKNN_TENSOR_NCHW) p3=true;
                else throw std::runtime_error("Unexpected output name/shape/layout; inspect attributes before proceeding");
            }
            if (!pred || !p3) throw std::runtime_error("Missing semantic output");
        }

        check(rknn_set_core_mask(ctx,RKNN_NPU_CORE_AUTO),"set_core_mask AUTO");
        unsigned pred_index=0,p3_index=0;
        for(const auto& a:logical_out) {
            if(std::string(a.name)=="predictions") pred_index=a.index;
            if(std::string(a.name)=="backbone_p3") p3_index=a.index;
        }
        double preload_ms=0;
        std::map<std::string,std::vector<float>> banks;
        std::vector<Job> jobs;std::ifstream input(argv[3]);std::string line;
        if(!input) throw std::runtime_error("Missing jobs file");
        while(std::getline(input,line)) {
            std::istringstream row(line);std::vector<std::string> f;std::string field;
            while(std::getline(row,field,'\t')) f.push_back(field);
            if(f.size()!=4) throw std::runtime_error("Expected ID,RGB,asset,expected_class");
            const std::map<std::string,int> expected={{"glass-insulator",7},{"lightning-rod-suspension",5},{"polymer-insulator-upper-shackle",11},{"vari-grip",9},{"yoke-suspension",1}};
            if(!expected.count(f[2]) || expected.at(f[2])!=std::stoi(f[3])) throw std::runtime_error("NO_BANK or asset-class mismatch");
            Job job;job.id=f[0];job.rgb=f[1];job.asset=f[2];job.cls=std::stoi(f[3]);
            if(rounds>1 || warmup) {
                const auto before=std::chrono::steady_clock::now();
                job.bytes=load_rgb(job.rgb);
                preload_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count();
            }
            jobs.push_back(std::move(job));
            if(!banks.count(f[2])) banks[f[2]]=load_bank(std::string(argv[2])+"/"+f[2]+".f32");
        }
        if(jobs.empty()) throw std::runtime_error("Empty jobs list");
        std::ofstream journal(argv[4],std::ios::app);journal.exceptions(std::ios::badbit|std::ios::failbit);
        auto run=[&](const Job& job,int round,bool record) {
            const auto read_start=std::chrono::steady_clock::now();
            std::vector<unsigned char> loaded;
            if(job.bytes.empty()) loaded=load_rgb(job.rgb);
            const auto& rgb=job.bytes.empty()?loaded:job.bytes;
            const auto start=std::chrono::steady_clock::now();
            const double read_ms=std::chrono::duration<double,std::milli>(start-read_start).count();
            rknn_input in{};in.index=0;in.buf=const_cast<unsigned char*>(rgb.data());in.size=rgb.size();
            in.type=RKNN_TENSOR_UINT8;in.fmt=RKNN_TENSOR_NHWC;in.pass_through=0;
            check(rknn_inputs_set(ctx,1,&in),"inputs_set");
            check(rknn_run(ctx,nullptr),"run");
            outputs.assign(io.n_output,rknn_output{});
            for(unsigned i=0;i<io.n_output;++i) {outputs[i].index=i;outputs[i].want_float=1;}
            check(rknn_outputs_get(ctx,io.n_output,outputs.data(),nullptr),"outputs_get");acquired=true;
            for(unsigned i=0;i<io.n_output;++i) if(outputs[i].size!=logical_out[i].n_elems*4) throw std::runtime_error("Output size mismatch");
            const auto model_done=std::chrono::steady_clock::now();
            const auto detections=insplad::decode_nms(static_cast<float*>(outputs[pred_index].buf),37800);
            const auto selection=insplad::select_asset(detections,job.cls);
            const auto post=insplad::postprocess_p3(static_cast<float*>(outputs[p3_index].buf),102400,selection.box);
            const auto roi_done=std::chrono::steady_clock::now();
            const auto& bank=banks.at(job.asset);
            const auto distances=insplad::nearest_euclidean(post.normalized_patches.data(),1600,bank.data(),1600,256);
            const double score=insplad::top8_score(distances);
            const auto score_done=std::chrono::steady_clock::now();
            const int release_ret=rknn_outputs_release(ctx,io.n_output,outputs.data());acquired=false;check(release_ret,"outputs_release");
            const auto end=std::chrono::steady_clock::now();
            if(record) {
                auto ms=[](auto a,auto b){return std::chrono::duration<double,std::milli>(b-a).count();};
                struct rusage usage{};getrusage(RUSAGE_SELF,&usage);
                journal << std::setprecision(17) << "{\"id\":" << std::quoted(job.id) << ",\"round\":" << round
                  << ",\"asset\":" << std::quoted(job.asset) << ",\"score\":" << score << ",\"box\":[";
                for(int k=0;k<4;++k) journal << (k?",":"") << selection.box[k];
                journal << "],\"confidence\":";
                if(selection.fallback) journal << "null";else journal << selection.confidence;
                journal << ",\"fallback\":" << (selection.fallback?"true":"false")
                  << ",\"rgb_read_ms\":" << read_ms << ",\"model_ms\":" << ms(start,model_done)
                  << ",\"detection_roi_ms\":" << ms(model_done,roi_done)
                  << ",\"retrieval_top8_ms\":" << ms(roi_done,score_done)
                  << ",\"rgb_to_score_ms\":" << ms(start,end) << ",\"maxrss_kib\":" << usage.ru_maxrss
                  << ",\"temperature_c\":" << temperature() << "}\n";
                journal.flush();
            }
        };
        for(int i=0;i<warmup;++i) run(jobs[i%jobs.size()],-1,false);
        {
            int actual_threads=0;
            std::ifstream status("/proc/self/status");std::string status_line;
            while(std::getline(status,status_line)) if(status_line.find("Threads:")==0) actual_threads=std::stoi(status_line.substr(8));
            std::ofstream system(output_path+".system.json");
            system << std::setprecision(17) << "{\"scoring_threads\":" << omp_get_max_threads()
              << ",\"actual_process_threads_after_warmup\":" << actual_threads
              << ",\"npu_core\":\"AUTO\",\"rgb_preload_ms\":" << preload_ms
              << ",\"rgb_preload_images\":" << ((rounds>1 || warmup)?jobs.size():0)
              << ",\"temperature_before_rounds_c\":" << temperature() << "}\n";
        }
        for(int round=0;round<rounds;++round) {
            std::size_t done=0;
            for(const auto& job:jobs) {run(job,round,true);if(++done%100==0) std::cout << "round " << round << " done " << done << '/' << jobs.size() << std::endl;}
        }
        const int ret=rknn_destroy(ctx);ctx=0;check(ret,"destroy");
    } catch(const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << std::endl;
        if(acquired) rknn_outputs_release(ctx,outputs.size(),outputs.data());
        if(ctx) rknn_destroy(ctx);
        return 2;
    }
    return 0;
}
