#!/usr/bin/env python3
"""Run upstream glmark2 discovery on normal-build staged resources, without a VM."""
from pathlib import Path
import subprocess,sys,tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from native_graphics_payload import native_graphics_payloads,stage_native_graphics_payloads
source=ROOT/'build/native-gallium/glmark2/src'
def function(file,signature):
 text=(source/file).read_text();start=text.index(signature)
 return text[start:text.index('\n}\n',start)+3]
code=r'''
#include <algorithm>
#include <cassert>
#include <cctype>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <iostream>
using std::string;using std::vector;
namespace fs=std::filesystem;
namespace Options {string data_path;}
namespace Util {
void list_files(const string& path,vector<string>& files){
 fs::path p(path);
 // BFS lookup ignores case, but readdir preserves the stored file name.
 if(!fs::exists(p)){
  string name=p.filename().string();
  std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return std::toupper(c);});
  p=p.parent_path()/name;
 }
 if(!fs::exists(p))return;
 for(const auto& e:fs::directory_iterator(p))if(e.is_regular_file())files.push_back(e.path().string());
}
}
enum ModelFormat {MODEL_INVALID,MODEL_3DS,MODEL_OBJ};
struct ModelDescriptor {string path;ModelDescriptor(const string&,ModelFormat,const string& p):path(p){}};
struct TextureDescriptor {
 enum FileType {FileTypeUnknown,FileTypePNG,FileTypeJPEG};
 string path;TextureDescriptor(const string&,const string& p,FileType):path(p){}
};
using ModelMap=std::map<string,ModelDescriptor*>;using TextureMap=std::map<string,TextureDescriptor*>;
namespace ModelPrivate {ModelMap modelMap;}
namespace TexturePrivate {TextureMap textureMap;}
struct Model {static const ModelMap& find_models();};
struct Texture {static const TextureMap& find_textures();};
''' + function('model.cpp','const ModelMap&\nModel::find_models()') + function('texture.cpp','const TextureMap&\nTexture::find_textures()') + r'''
int main(int argc,char** argv){
 assert(argc==3);Options::data_path=argv[1];
 const auto& models=Model::find_models();const auto& textures=Texture::find_textures();
 if(string(argv[2])=="broken"){
  assert(models.empty() && textures.empty());
  std::cout<<"Reproduced: uppercased package makes upstream discover zero models/textures\n";
 }else{
  for(const char* key:{"horse","cat","bunny"})assert(models.count(key) && fs::exists(models.at(key)->path));
  assert(textures.count("jellyfish256") && fs::exists(textures.at("jellyfish256")->path));
  assert(textures.count("jellyfish-caustics-01"));
  assert(fs::exists(fs::path(Options::data_path)/"models/jellyfish.jobj"));
  assert(fs::exists(fs::path(Options::data_path)/"shaders/jellyfish.frag"));
  std::cout<<"PASS: original upstream discovery recognizes "<<models.size()<<" models and "<<textures.size()<<" textures\n";
 }
 for(const auto& e:models)delete e.second;
 for(const auto& e:textures)delete e.second;
}
'''
with tempfile.TemporaryDirectory(prefix='bk-glmark-resources-') as folder:
 p=Path(folder);cpp=p/'discovery.cpp';cpp.write_text(code);binary=p/'discovery'
 subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror',str(cpp),'-o',str(binary)],check=True)
 files={guest:host for guest,host in native_graphics_payloads().items() if guest.startswith('/SYSTEM/SHARE/GLMARK2/')}
 # Use the real normal-build staging function with the manifest's real assets.
 stage_native_graphics_payloads(p/'fixed',files)
 stage_native_graphics_payloads(p/'broken',{guest.upper():host for guest,host in files.items()})
 for name in ['broken','fixed']:
  subprocess.run([str(binary),str(p/name/'SYSTEM/SHARE/GLMARK2'),name],check=True)
