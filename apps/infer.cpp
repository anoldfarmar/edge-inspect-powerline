#include "edgeinspect/inference_engine.h"
#include <sys/resource.h>
#include <charconv>
#include <chrono>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {
std::uint64_t positive(const std::string& s) {
    std::uint64_t n=0; 
    const auto result=std::from_chars(s.data(),s .data()+s.size(),n);

    if(result.ec!=std::errc{} || result.ptr!=s.data()+s.size() || !n) throw std::invalid_argument("Expected positive integer: "+s);
    
    return n;
}
void usage() {
    std::cout<<"EdgeInspect stage1\n"
      "  edgeinspect_cli --root PROJECT --image FILE --asset ASSET --output NEW.json\n"
      "  edgeinspect_cli --root PROJECT --rgb EXACT.rgb --asset ASSET --output NEW.json\n"
      "Options: --diagnostics (NEW.json.tensors/), --rgb-out NEW.rgb, --repeat N\n"
      "         --max-bytes N (default 33554432), --max-pixels N (default 24000000)\n"
      "         --preprocess-only (no model; requires --rgb-out, no asset needed)\n"
      "No anomaly threshold is configured. Output score + null decision. JPEG/8-bit nonpalette PNG only.\n";
}
}

int main(int argc,char** argv) {
    try {
        // edgeinspect_cli --image samples/normal.jpg --asset glass-insulator
        std::map<std::string,std::string> args; bool diagnostic=false, preprocess_only=false;
        for(int i=1;i<argc;++i) {
            const std::string key=argv[i];
            if(key=="--help") {usage();return 0;}
            if(key=="--diagnostics") {diagnostic=true;continue;}
            if(key=="--preprocess-only") {preprocess_only=true;continue;}
            if(key!="--root" && key!="--image" && key!="--rgb" && key!="--asset" && key!="--output" &&
               key!="--rgb-out" && key!="--repeat" && key!="--max-bytes" && key!="--max-pixels")
                throw std::invalid_argument("Unknown option: "+key);
            if(++i==argc || args.count(key)) throw std::invalid_argument("Missing/duplicate argument: "+key);
            args[key]=argv[i];
        }
        if(args.count("--image")+args.count("--rgb")!=1 || !args.count("--output")) throw std::invalid_argument("Need exactly one --image/--rgb and --output; see --help");
        if(preprocess_only && !args.count("--rgb-out")) throw std::invalid_argument("--preprocess-only requires --rgb-out");
        if(!preprocess_only) {if(!args.count("--asset")) throw std::invalid_argument("--asset is required"); edgeinspect::asset_class(args.at("--asset"));}
        edgeinspect::Config config; config.root=args.count("--root")?std::filesystem::path(args.at("--root")):std::filesystem::current_path();
        if(args.count("--max-pixels")) config.max_image_pixels=positive(args.at("--max-pixels"));
        if(args.count("--max-bytes")) config.max_image_bytes=positive(args.at("--max-bytes"));
        const auto repeat=args.count("--repeat")?positive(args.at("--repeat")):1;
        if(repeat>10000 || (preprocess_only && (repeat!=1 || diagnostic))) throw std::invalid_argument("Invalid repeat/diagnostics for this mode");
        const std::filesystem::path output=args.at("--output");
        if(std::filesystem::exists(output)) throw std::runtime_error("Result exists; choose a NEW output path");
        if(args.count("--rgb-out") && std::filesystem::exists(args.at("--rgb-out"))) throw std::runtime_error("RGB output exists");
        auto input=args.count("--image")?edgeinspect::prepare_image(args.at("--image"),config):edgeinspect::prepare_rgb(args.at("--rgb"));
        if(args.count("--rgb-out")) edgeinspect::save_new_file(args.at("--rgb-out"),input.rgb.data(),input.rgb.size());
        std::ostringstream json;
        if(preprocess_only) {
            json<<"{\"rgb_sha256\":"<<edgeinspect::json_string(input.rgb_sha256)<<",\"input_sha256\":"<<edgeinspect::json_string(input.input_sha256)
                <<",\"original_wh\":["<<input.original_width<<','<<input.original_height<<"],\"preprocessing_ms\":"<<input.preprocessing_ms<<"}\n";
        } else {
            const auto begin=std::chrono::steady_clock::now();
            edgeinspect::InferenceEngine engine(config);
            const double init_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
            json<<"{\"engine_initializations\":1,\"initialization_ms\":"<<init_ms
                <<",\"input_prepared_once\":true,\"runs\":[";
            for(std::uint64_t i=0;i<repeat;++i) {
                const auto result=engine.infer(input,args.at("--asset"),diagnostic);
                if(i) json<<',';
                json<<edgeinspect::result_json(result,input,engine.environment_json());
                if(diagnostic) {
                    auto dir=std::filesystem::path(output.string()+".tensors");
                    if(repeat>1) dir+=std::string(".")+std::to_string(i);
                    if(!std::filesystem::create_directory(dir)) throw std::runtime_error("Diagnostic directory exists");
                    const auto& d=*result.diagnostics;
                    const std::pair<const char*,const std::vector<float>*> tensors[]={
                        {"predictions.f32",&d.predictions},{"backbone_p3.f32",&d.p3},
                        {"roi.f32",&d.roi},{"normalized_patches.f32",&d.normalized_patches}};
                    for(const auto& [name,values]:tensors) edgeinspect::save_new_file(dir/name,values->data(),values->size()*sizeof(float));
                }
            }
            struct rusage memory{}; getrusage(RUSAGE_SELF,&memory);
            json<<"],\"peak_rss_kib\":"<<memory.ru_maxrss<<"}\n";
        }
        const auto text=json.str(); edgeinspect::save_new_file(output,text.data(),text.size());
        std::cout<<"Saved "<<output<<'\n';
        return 0;
    } catch(const std::exception& e) {std::cerr<<"EdgeInspect failed: "<<e.what()<<'\n';return 2;}
}
