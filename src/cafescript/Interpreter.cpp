#include "cafe/cafescript/Interpreter.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <unordered_set>

namespace cafe::script {

bool Value::truthy() const {
    if (std::holds_alternative<std::monostate>(data)) return false;
    if (std::holds_alternative<NullValue>(data)) return false;
    if (auto v = std::get_if<bool>(&data)) return *v;
    if (auto v = std::get_if<double>(&data)) return *v != 0;
    if (auto v = std::get_if<std::string>(&data)) return !v->empty();
    return true;
}
double Value::asNumber() const {
    if (auto v = std::get_if<double>(&data)) return *v;
    if (auto v = std::get_if<bool>(&data)) return *v ? 1 : 0;
    if (auto v = std::get_if<std::string>(&data)) {
        try { return std::stod(*v); } catch (...) {}
    }
    return 0;
}
std::string Value::asString() const {
    if (auto v = std::get_if<std::string>(&data)) return *v;
    if (auto v = std::get_if<double>(&data)) {
        auto s=std::to_string(*v);
        while(s.size()>1&&s.back()=='0')s.pop_back();
        if(s.back()=='.')s.pop_back();
        return s;
    }
    if (auto v=std::get_if<bool>(&data)) return *v?"true":"false";
    if (std::holds_alternative<std::monostate>(data)) return "undefined";
    if (std::holds_alternative<NullValue>(data)) return "null";
    if (auto values=std::get_if<std::shared_ptr<Array>>(&data)) {
        std::string result;
        for (std::size_t i=0;i<(*values)->size();++i) { if(i)result+=',';result+=(*values)->at(i).asString(); }
        return result;
    }
    if (std::holds_alternative<std::shared_ptr<Function>>(data)) return "function";
    if (std::holds_alternative<Node*>(data)) return "[object Node]";
    return "[object Object]";
}

namespace {
enum class TK { End, Identifier, Number, String, Template, Let, Const, Var, Function, If, Else, While, For,
    Return, Break, Continue, True, False, Null, Undefined, Typeof, Of, On,
    LParen,RParen,LBrace,RBrace,LBracket,RBracket,Comma,Dot,Colon,Question,Semicolon,Hash,Arrow,
    Plus,Minus,Star,Slash,Percent,Bang,Equal,Less,Greater,PlusPlus,MinusMinus,
    EqualEqual,BangEqual,StrictEqual,StrictBangEqual,LessEqual,GreaterEqual,AndAnd,OrOr,
    PlusEqual,MinusEqual,StarEqual,SlashEqual,Power };
struct Token { TK kind; std::string text; double number{0}; int line{1}; };

class Lexer {
public:
    explicit Lexer(std::string_view s):s_(s){}
    std::vector<Token> scan() { std::vector<Token> out; for(;;){auto t=next();out.push_back(t);if(t.kind==TK::End)return out;} }
private:
    std::string_view s_; std::size_t p_{0}; int line_{1};
    Token next() {
        while(p_<s_.size()) { if(s_[p_]=='\n'){++line_;++p_;} else if(std::isspace(static_cast<unsigned char>(s_[p_])))++p_;
            else if(s_.substr(p_,2)=="//"){while(p_<s_.size()&&s_[p_]!='\n')++p_;}
            else if(s_.substr(p_,2)=="/*"){p_+=2;while(p_+1<s_.size()&&s_.substr(p_,2)!="*/"){if(s_[p_++]=='\n')++line_;}p_=std::min(s_.size(),p_+2);}
            else break; }
        if(p_>=s_.size())return{TK::End,"",0,line_};
        char c=s_[p_++];
        if(std::isalpha(static_cast<unsigned char>(c))||c=='_') { std::size_t a=p_-1;while(p_<s_.size()&&(std::isalnum(static_cast<unsigned char>(s_[p_]))||s_[p_]=='_'))++p_;std::string w(s_.substr(a,p_-a));
            static const std::unordered_map<std::string,TK> kw{{"let",TK::Let},{"const",TK::Const},{"var",TK::Var},{"function",TK::Function},{"if",TK::If},{"else",TK::Else},{"while",TK::While},{"for",TK::For},{"return",TK::Return},{"break",TK::Break},{"continue",TK::Continue},{"true",TK::True},{"false",TK::False},{"null",TK::Null},{"undefined",TK::Undefined},{"typeof",TK::Typeof},{"of",TK::Of},{"on",TK::On}};
            auto it=kw.find(w);return{it==kw.end()?TK::Identifier:it->second,w,0,line_}; }
        if(std::isdigit(static_cast<unsigned char>(c))||(c=='.'&&p_<s_.size()&&std::isdigit(static_cast<unsigned char>(s_[p_])))){std::size_t a=p_-1;while(p_<s_.size()&&(std::isdigit(static_cast<unsigned char>(s_[p_]))||s_[p_]=='.'))++p_;std::string n(s_.substr(a,p_-a));return{TK::Number,n,std::stod(n),line_};}
        if(c=='"'||c=='\''){char q=c;std::string v;while(p_<s_.size()&&s_[p_]!=q){char x=s_[p_++];if(x=='\\'&&p_<s_.size()){char e=s_[p_++];v+=e=='n'?'\n':e=='t'?'\t':e;}else v+=x;}if(p_<s_.size())++p_;return{TK::String,v,0,line_};}
        if(c=='`'){std::string v;while(p_<s_.size()&&s_[p_]!='`'){char x=s_[p_++];if(x=='\n')++line_;if(x=='\\'&&p_<s_.size()){char e=s_[p_++];v+=e=='n'?'\n':e=='t'?'\t':e;}else v+=x;}if(p_<s_.size())++p_;return{TK::Template,v,0,line_};}
        auto two=[&](char expected,TK yes,TK no){if(p_<s_.size()&&s_[p_]==expected){++p_;return Token{yes,std::string{s_[p_-2],s_[p_-1]},0,line_};}return Token{no,std::string(1,c),0,line_};};
        switch(c){case'(':return{TK::LParen,"(",0,line_};case')':return{TK::RParen,")",0,line_};case'{':return{TK::LBrace,"{",0,line_};case'}':return{TK::RBrace,"}",0,line_};case'[':return{TK::LBracket,"[",0,line_};case']':return{TK::RBracket,"]",0,line_};case',':return{TK::Comma,",",0,line_};case'.':return{TK::Dot,".",0,line_};case':':return{TK::Colon,":",0,line_};case'?':return{TK::Question,"?",0,line_};case';':return{TK::Semicolon,";",0,line_};case'#':return{TK::Hash,"#",0,line_};case'+':if(p_<s_.size()&&s_[p_]=='+'){++p_;return{TK::PlusPlus,"++",0,line_};}return two('=',TK::PlusEqual,TK::Plus);case'-':if(p_<s_.size()&&s_[p_]=='-'){++p_;return{TK::MinusMinus,"--",0,line_};}return two('=',TK::MinusEqual,TK::Minus);case'*':if(p_<s_.size()&&s_[p_]=='*'){++p_;return{TK::Power,"**",0,line_};}return two('=',TK::StarEqual,TK::Star);case'/':return two('=',TK::SlashEqual,TK::Slash);case'%':return{TK::Percent,"%",0,line_};case'!':if(p_<s_.size()&&s_[p_]=='='){++p_;if(p_<s_.size()&&s_[p_]=='='){++p_;return{TK::StrictBangEqual,"!==",0,line_};}return{TK::BangEqual,"!=",0,line_};}return{TK::Bang,"!",0,line_};case'=':if(p_<s_.size()&&s_[p_]=='>'){++p_;return{TK::Arrow,"=>",0,line_};}if(p_<s_.size()&&s_[p_]=='='){++p_;if(p_<s_.size()&&s_[p_]=='='){++p_;return{TK::StrictEqual,"===",0,line_};}return{TK::EqualEqual,"==",0,line_};}return{TK::Equal,"=",0,line_};case'<':return two('=',TK::LessEqual,TK::Less);case'>':return two('=',TK::GreaterEqual,TK::Greater);case'&':return two('&',TK::AndAnd,TK::End);case'|':return two('|',TK::OrOr,TK::End);default:throw std::runtime_error("Unexpected CafeScript character on line "+std::to_string(line_));}
    }
};
}

struct Runtime;
struct Expr { virtual ~Expr()=default; virtual Value eval(Runtime&)=0; };
enum class ExecFlow { Normal, Return, Break, Continue };
struct ExecResult { ExecFlow flow{ExecFlow::Normal}; Value value; };
struct Stmt { virtual ~Stmt()=default; virtual ExecResult exec(Runtime&)=0; };

namespace {
struct Environment {
    std::unordered_map<std::string,Value> values;
    std::unordered_set<std::string> constants;
    std::shared_ptr<Environment> parent;
    Value get(const std::string& n) const { auto it=values.find(n);if(it!=values.end())return it->second;return parent?parent->get(n):Value{}; }
    void define(std::string n,Value v,bool constant=false){values[n]=std::move(v);if(constant)constants.insert(std::move(n));}
    bool assign(const std::string&n,Value v){auto it=values.find(n);if(it!=values.end()){if(constants.contains(n))throw std::runtime_error("Cannot assign to const '"+n+"'");it->second=std::move(v);return true;}return parent?parent->assign(n,std::move(v)):false;}
};
}

Value makeNative(std::function<Value(const std::vector<Value>&)> callback) {
    auto function = std::make_shared<Function>();
    function->native = std::move(callback);
    Value value;
    value.data = function;
    return value;
}

bool strictEqual(const Value& left, const Value& right) {
    if (left.data.index() != right.data.index()) return false;
    if (auto value = std::get_if<double>(&left.data)) return *value == std::get<double>(right.data);
    if (auto value = std::get_if<std::string>(&left.data)) return *value == std::get<std::string>(right.data);
    if (auto value = std::get_if<bool>(&left.data)) return *value == std::get<bool>(right.data);
    if (auto value = std::get_if<Node*>(&left.data)) return *value == std::get<Node*>(right.data);
    if (auto value = std::get_if<std::shared_ptr<Value::Array>>(&left.data)) return *value == std::get<std::shared_ptr<Value::Array>>(right.data);
    if (auto value = std::get_if<std::shared_ptr<Object>>(&left.data)) return *value == std::get<std::shared_ptr<Object>>(right.data);
    if (auto value = std::get_if<std::shared_ptr<Function>>(&left.data)) return *value == std::get<std::shared_ptr<Function>>(right.data);
    return true;
}

std::string typeOf(const Value& value) {
    if (std::holds_alternative<double>(value.data)) return "number";
    if (std::holds_alternative<std::string>(value.data)) return "string";
    if (std::holds_alternative<bool>(value.data)) return "boolean";
    if (std::holds_alternative<std::shared_ptr<Function>>(value.data)) return "function";
    if (std::holds_alternative<std::monostate>(value.data)) return "undefined";
    if (std::holds_alternative<NullValue>(value.data)) return "object";
    return "object";
}

struct Runtime {
    SceneGraph& graph; InputSystem& input;
    std::shared_ptr<Environment> globals;
    std::shared_ptr<Environment> env;
    // Most function and block scopes die in the same frame. Recycle only
    // scopes with no surviving closure, so captured variables keep their
    // original lifetime and semantics.
    std::vector<std::shared_ptr<Environment>> freeScopes;
    std::unordered_map<std::string,std::vector<std::shared_ptr<Function>>> events;
    Interpreter::EventCallback* sceneOpen{nullptr};
    Interpreter::NodeCallback* spawnHook{nullptr};
    Interpreter::AudioPlayCallback* audioPlay{nullptr};
    Interpreter::AudioStopCallback* audioStop{nullptr};
    Interpreter::AudioControlCallback* audioControl{nullptr};
    Interpreter::AudioStateCallback* audioState{nullptr};
    Interpreter::StorageReadCallback* storageRead{nullptr};
    Interpreter::StorageWriteCallback* storageWrite{nullptr};
    Interpreter::StorageActionCallback* storageExists{nullptr};
    Interpreter::StorageActionCallback* storageRemove{nullptr};
    Interpreter::AlphaHitCallback* alphaHit{nullptr};
    Interpreter::MaskEdgesCallback* maskEdges{nullptr};
    Runtime(SceneGraph& g, InputSystem& i) : graph(g), input(i),
        globals(std::make_shared<Environment>()), env(globals) {}
    std::shared_ptr<Environment> acquireScope(const std::shared_ptr<Environment>& parent) {
        std::shared_ptr<Environment> scope;
        if (!freeScopes.empty()) {
            scope=std::move(freeScopes.back());
            freeScopes.pop_back();
            scope->values.clear();
            scope->constants.clear();
        } else scope=std::make_shared<Environment>();
        scope->parent=parent;
        return scope;
    }
    void releaseScope(std::shared_ptr<Environment> scope) {
        if (scope.use_count()==1 && freeScopes.size()<256) {
            scope->parent.reset();
            freeScopes.push_back(std::move(scope));
        }
    }
    Value getMember(const Value& object,const std::string& name) {
        if(auto o=std::get_if<std::shared_ptr<Object>>(&object.data)){auto it=(*o)->fields.find(name);return it==(*o)->fields.end()?Value{}:it->second;}
        if(auto n=std::get_if<Node*>(&object.data)){if(!*n)return{};Node& x=**n;if(name=="x")return x.transform3D.position.x;if(name=="y")return x.transform3D.position.y;if(name=="z")return x.transform3D.position.z;if(name=="rotation")return x.transform.rotation;if(name=="rotationX")return x.transform3D.rotation.x;if(name=="rotationY")return x.transform3D.rotation.y;if(name=="rotationZ")return x.transform3D.rotation.z;if(name=="scale")return x.transform.scale.x;if(name=="scaleX")return x.transform3D.scale.x;if(name=="scaleY")return x.transform3D.scale.y;if(name=="scaleZ")return x.transform3D.scale.z;if(name=="opacity")return x.opacity;if(name=="hue")return x.hue;if(name=="saturation")return x.saturation;if(name=="visible")return x.visible;if(name=="text")return x.text;if(name=="disabled")return x.disabled;if(name=="id")return x.id;if(name=="width")return x.width;if(name=="height")return x.height;if(name=="depth")return x.depth;if(name=="fov")return x.fieldOfView;if(name=="emitting")return x.emitter.emitting;if(name=="rate")return x.emitter.rate;if(name=="particleSize")return x.emitter.particleSize;if(name=="endSize")return x.emitter.endSize;if(name=="particleAspect")return x.emitter.aspect;if(name=="spin")return x.emitter.angularVelocity;if(name=="particleShape")return x.emitter.shape==ParticleShape::Rectangle?"rectangle":"circle";if(name=="particleCount")return static_cast<double>(x.emitter.particles.size());if(name=="count"&&x.type==NodeType::SpriteBatch)return static_cast<double>(x.batch.sprites.size());if(name=="add"&&x.type==NodeType::SpriteBatch){Node*batch=&x;return makeNative([this,batch](const auto&args)->Value{if(args.empty()||batch->batch.sprites.size()>=batch->batch.maximum)return-1.0;SpriteInstance item;auto option=[&](const char*key,Value fallback){if(!std::holds_alternative<std::shared_ptr<Object>>(args[0].data))return fallback;Value found=getMember(args[0],key);return std::holds_alternative<std::monostate>(found.data)?fallback:found;};if(std::holds_alternative<std::shared_ptr<Object>>(args[0].data)){item.x=option("x",0.0).asNumber();item.y=option("y",0.0).asNumber();item.width=option("width",0.0).asNumber();item.height=option("height",0.0).asNumber();item.rotation=option("rotation",0.0).asNumber();item.scaleX=option("scaleX",1.0).asNumber();item.scaleY=option("scaleY",1.0).asNumber();item.opacity=option("opacity",1.0).asNumber();item.visible=option("visible",true).truthy();if(auto color=Color::parse(option("tint","#ffffffff").asString()))item.tint=*color;}else{item.x=args[0].asNumber();if(args.size()>1)item.y=args[1].asNumber();if(args.size()>2)item.rotation=args[2].asNumber();if(args.size()>3)item.scaleX=item.scaleY=args[3].asNumber();}batch->batch.sprites.push_back(item);++batch->batch.revision;return static_cast<double>(batch->batch.sprites.size()-1);});}if(name=="set"&&x.type==NodeType::SpriteBatch){Node*batch=&x;return makeNative([this,batch](const auto&args)->Value{if(args.size()<2)return false;const int index=static_cast<int>(args[0].asNumber());if(index<0||index>=static_cast<int>(batch->batch.sprites.size())||!std::holds_alternative<std::shared_ptr<Object>>(args[1].data))return false;SpriteInstance&item=batch->batch.sprites[static_cast<std::size_t>(index)];auto apply=[&](const char*key,double&field){Value value=getMember(args[1],key);if(!std::holds_alternative<std::monostate>(value.data))field=value.asNumber();};apply("x",item.x);apply("y",item.y);apply("width",item.width);apply("height",item.height);apply("rotation",item.rotation);apply("scaleX",item.scaleX);apply("scaleY",item.scaleY);apply("opacity",item.opacity);Value visible=getMember(args[1],"visible");if(!std::holds_alternative<std::monostate>(visible.data))item.visible=visible.truthy();Value tint=getMember(args[1],"tint");if(auto color=Color::parse(tint.asString()))item.tint=*color;++batch->batch.revision;return true;});}if(name=="clear"&&x.type==NodeType::SpriteBatch){Node*batch=&x;return makeNative([batch](const auto&)->Value{batch->batch.sprites.clear();++batch->batch.revision;return{};});}if(name=="remove"&&x.type==NodeType::SpriteBatch){Node*batch=&x;return makeNative([batch](const auto&args)->Value{if(args.empty())return false;const int index=static_cast<int>(args[0].asNumber());if(index<0||index>=static_cast<int>(batch->batch.sprites.size()))return false;batch->batch.sprites.erase(batch->batch.sprites.begin()+index);++batch->batch.revision;return true;});}if(name=="src"||name=="source")return x.source;}
        if(auto a=std::get_if<std::shared_ptr<Value::Array>>(&object.data)){
            auto values=*a;
            if(name=="length")return static_cast<double>(values->size());
            if(name=="push")return makeNative([values](const auto&args){for(const auto&value:args)values->push_back(value);return Value(static_cast<double>(values->size()));});
            if(name=="pop")return makeNative([values](const auto&){if(values->empty())return Value{};Value value=values->back();values->pop_back();return value;});
            if(name=="shift")return makeNative([values](const auto&){if(values->empty())return Value{};Value value=values->front();values->erase(values->begin());return value;});
            if(name=="unshift")return makeNative([values](const auto&args){values->insert(values->begin(),args.begin(),args.end());return Value(static_cast<double>(values->size()));});
            if(name=="join")return makeNative([values](const auto&args){const std::string separator=args.empty()?",":args[0].asString();std::string result;for(std::size_t i=0;i<values->size();++i){if(i)result+=separator;result+=(*values)[i].asString();}return Value(result);});
            if(name=="includes")return makeNative([values](const auto&args){if(args.empty())return Value(false);for(const auto&value:*values)if(strictEqual(value,args[0]))return Value(true);return Value(false);});
            if(name=="indexOf")return makeNative([values](const auto&args){if(args.empty())return Value(-1.0);for(std::size_t i=0;i<values->size();++i)if(strictEqual((*values)[i],args[0]))return Value(static_cast<double>(i));return Value(-1.0);});
            if(name=="forEach")return makeNative([this,values](const auto&args){if(args.empty())return Value{};for(std::size_t i=0;i<values->size();++i)call(args[0],{(*values)[i],Value(static_cast<double>(i))});return Value{};});
            if(name=="map")return makeNative([this,values](const auto&args){auto output=std::make_shared<Value::Array>();if(!args.empty())for(std::size_t i=0;i<values->size();++i)output->push_back(call(args[0],{(*values)[i],Value(static_cast<double>(i))}));Value result;result.data=output;return result;});
            if(name=="filter")return makeNative([this,values](const auto&args){auto output=std::make_shared<Value::Array>();if(!args.empty())for(std::size_t i=0;i<values->size();++i)if(call(args[0],{(*values)[i],Value(static_cast<double>(i))}).truthy())output->push_back((*values)[i]);Value result;result.data=output;return result;});
            if(name=="reduce")return makeNative([this,values](const auto&args){if(args.empty()||(values->empty()&&args.size()<2))return Value{};std::size_t i=args.size()>1?0:1;Value result=args.size()>1?args[1]:(*values)[0];for(;i<values->size();++i)result=call(args[0],{result,(*values)[i],Value(static_cast<double>(i))});return result;});
            if(name=="find")return makeNative([this,values](const auto&args){if(args.empty())return Value{};for(std::size_t i=0;i<values->size();++i)if(call(args[0],{(*values)[i],Value(static_cast<double>(i))}).truthy())return (*values)[i];return Value{};});
            if(name=="findIndex")return makeNative([this,values](const auto&args){if(args.empty())return Value(-1.0);for(std::size_t i=0;i<values->size();++i)if(call(args[0],{(*values)[i],Value(static_cast<double>(i))}).truthy())return Value(static_cast<double>(i));return Value(-1.0);});
            if(name=="some")return makeNative([this,values](const auto&args){if(args.empty())return Value(false);for(std::size_t i=0;i<values->size();++i)if(call(args[0],{(*values)[i],Value(static_cast<double>(i))}).truthy())return Value(true);return Value(false);});
            if(name=="every")return makeNative([this,values](const auto&args){if(args.empty())return Value(false);for(std::size_t i=0;i<values->size();++i)if(!call(args[0],{(*values)[i],Value(static_cast<double>(i))}).truthy())return Value(false);return Value(true);});
            if(name=="slice")return makeNative([values](const auto&args){int size=static_cast<int>(values->size()),start=args.empty()?0:static_cast<int>(args[0].asNumber()),end=args.size()<2?size:static_cast<int>(args[1].asNumber());if(start<0)start=std::max(0,size+start);if(end<0)end=std::max(0,size+end);start=std::clamp(start,0,size);end=std::clamp(end,start,size);auto output=std::make_shared<Value::Array>(values->begin()+start,values->begin()+end);Value result;result.data=output;return result;});
            if(name=="splice")return makeNative([values](const auto&args){const int size=static_cast<int>(values->size());int start=args.empty()?0:static_cast<int>(args[0].asNumber());if(start<0)start=std::max(0,size+start);start=std::clamp(start,0,size);int count=args.size()<2?size-start:std::clamp(static_cast<int>(args[1].asNumber()),0,size-start);auto removed=std::make_shared<Value::Array>(values->begin()+start,values->begin()+start+count);values->erase(values->begin()+start,values->begin()+start+count);if(args.size()>2)values->insert(values->begin()+start,args.begin()+2,args.end());Value result;result.data=removed;return result;});
            if(name=="concat")return makeNative([values](const auto&args){auto output=std::make_shared<Value::Array>(*values);for(const Value&value:args){if(auto array=std::get_if<std::shared_ptr<Value::Array>>(&value.data))output->insert(output->end(),(*array)->begin(),(*array)->end());else output->push_back(value);}Value result;result.data=output;return result;});
            if(name=="fill")return makeNative([values](const auto&args){if(args.empty()){Value result;result.data=values;return result;}const int size=static_cast<int>(values->size());int start=args.size()>1?static_cast<int>(args[1].asNumber()):0,end=args.size()>2?static_cast<int>(args[2].asNumber()):size;if(start<0)start=std::max(0,size+start);if(end<0)end=std::max(0,size+end);for(int i=std::clamp(start,0,size);i<std::clamp(end,0,size);++i)(*values)[static_cast<std::size_t>(i)]=args[0];Value result;result.data=values;return result;});
            if(name=="reverse")return makeNative([values](const auto&){std::reverse(values->begin(),values->end());Value result;result.data=values;return result;});
        }
        if(auto text=std::get_if<std::string>(&object.data)){
            const std::string value=*text;
            if(name=="length")return static_cast<double>(value.size());
            if(name=="includes")return makeNative([value](const auto&args){return Value(!args.empty()&&value.find(args[0].asString())!=std::string::npos);});
            if(name=="startsWith")return makeNative([value](const auto&args){const auto prefix=args.empty()?std::string{}:args[0].asString();return Value(value.starts_with(prefix));});
            if(name=="endsWith")return makeNative([value](const auto&args){const auto suffix=args.empty()?std::string{}:args[0].asString();return Value(value.ends_with(suffix));});
            if(name=="toUpperCase")return makeNative([value](const auto&){auto output=value;for(char&c:output)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));return Value(output);});
            if(name=="toLowerCase")return makeNative([value](const auto&){auto output=value;for(char&c:output)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return Value(output);});
            if(name=="trim")return makeNative([value](const auto&){return Value(cafe::trim(value));});
            if(name=="slice"||name=="substring")return makeNative([value,name](const auto&args){int size=static_cast<int>(value.size()),start=args.empty()?0:static_cast<int>(args[0].asNumber()),end=args.size()<2?size:static_cast<int>(args[1].asNumber());if(name=="slice"){if(start<0)start=std::max(0,size+start);if(end<0)end=std::max(0,size+end);}else{start=std::max(0,start);end=std::max(0,end);if(start>end)std::swap(start,end);}start=std::clamp(start,0,size);end=std::clamp(end,start,size);return Value(value.substr(static_cast<std::size_t>(start),static_cast<std::size_t>(end-start)));});
            if(name=="split")return makeNative([value](const auto&args){auto output=std::make_shared<Value::Array>();const std::string separator=args.empty()?std::string{}:args[0].asString();if(separator.empty()){for(char c:value)output->emplace_back(std::string(1,c));}else{std::size_t at=0,next=0;while((next=value.find(separator,at))!=std::string::npos){output->emplace_back(value.substr(at,next-at));at=next+separator.size();}output->emplace_back(value.substr(at));}Value result;result.data=output;return result;});
        }
        return{};
    }
    Value getIndex(const Value& object,const Value& index) {
        if(auto a=std::get_if<std::shared_ptr<Value::Array>>(&object.data)){const int i=static_cast<int>(index.asNumber());return i>=0&&i<static_cast<int>((*a)->size())?(*a)->at(static_cast<std::size_t>(i)):Value{};}
        if(std::holds_alternative<std::shared_ptr<Object>>(object.data))return getMember(object,index.asString());
        if(auto text=std::get_if<std::string>(&object.data)){const int i=static_cast<int>(index.asNumber());return i>=0&&i<static_cast<int>(text->size())?Value(std::string(1,(*text)[static_cast<std::size_t>(i)])):Value{};}
        return{};
    }
    void setMember(const Value& object,const std::string& name,Value value) {
        if(auto o=std::get_if<std::shared_ptr<Object>>(&object.data)){(*o)->fields[name]=std::move(value);return;}
        if(auto n=std::get_if<Node*>(&object.data)){if(!*n)return;Node& x=**n;if(name=="x"){x.transform.position.x=value.asNumber();x.transform3D.position.x=value.asNumber();}else if(name=="y"){x.transform.position.y=value.asNumber();x.transform3D.position.y=value.asNumber();}else if(name=="z")x.transform3D.position.z=value.asNumber();else if(name=="rotation"){x.transform.rotation=value.asNumber();x.transform3D.rotation.z=value.asNumber();}else if(name=="rotationX")x.transform3D.rotation.x=value.asNumber();else if(name=="rotationY")x.transform3D.rotation.y=value.asNumber();else if(name=="rotationZ")x.transform3D.rotation.z=value.asNumber();else if(name=="scale"){x.transform.scale={value.asNumber(),value.asNumber()};x.transform3D.scale={value.asNumber(),value.asNumber(),value.asNumber()};}else if(name=="scaleX"){x.transform.scale.x=value.asNumber();x.transform3D.scale.x=value.asNumber();}else if(name=="scaleY"){x.transform.scale.y=value.asNumber();x.transform3D.scale.y=value.asNumber();}else if(name=="scaleZ")x.transform3D.scale.z=value.asNumber();else if(name=="opacity")x.opacity=value.asNumber();else if(name=="hue")x.hue=value.asNumber();else if(name=="saturation")x.saturation=value.asNumber();else if(name=="visible")x.visible=value.truthy();else if(name=="text")x.text=value.asString();else if(name=="disabled")x.disabled=value.truthy();else if(name=="width")x.width=value.asNumber();else if(name=="height")x.height=value.asNumber();else if(name=="depth")x.depth=value.asNumber();else if(name=="fov")x.fieldOfView=value.asNumber();else if(name=="emitting")x.emitter.emitting=value.truthy();else if(name=="rate")x.emitter.rate=value.asNumber();else if(name=="particleSize")x.emitter.particleSize=value.asNumber();else if(name=="endSize")x.emitter.endSize=value.asNumber();else if(name=="particleAspect")x.emitter.aspect=value.asNumber();else if(name=="spin")x.emitter.angularVelocity=value.asNumber();else if(name=="particleShape")x.emitter.shape=value.asString()=="rectangle"?ParticleShape::Rectangle:ParticleShape::Circle;else if(name=="tint"){if(auto color=Color::parse(value.asString()))x.tint=*color;}else if(name=="src"||name=="source")x.source=value.asString();}
    }
    void setIndex(const Value& object,const Value& index,Value value) {
        if(auto a=std::get_if<std::shared_ptr<Value::Array>>(&object.data)){const int i=static_cast<int>(index.asNumber());if(i<0)return;if(i>=static_cast<int>((*a)->size()))(*a)->resize(static_cast<std::size_t>(i+1));(*a)->at(static_cast<std::size_t>(i))=std::move(value);return;}
        if(std::holds_alternative<std::shared_ptr<Object>>(object.data))setMember(object,index.asString(),std::move(value));
    }
    Value call(Value callee,const std::vector<Value>& args) {
        auto fp=std::get_if<std::shared_ptr<Function>>(&callee.data);if(!fp||!*fp)return{};auto& f=**fp;if(f.native)return f.native(args);
        auto closure=std::static_pointer_cast<Environment>(f.closure);
        auto local=acquireScope(closure?closure:globals);for(std::size_t i=0;i<f.params.size();++i)local->define(f.params[i],i<args.size()?args[i]:Value{});auto old=env;env=local;Value result;
        for(auto&s:f.body){auto execution=s->exec(*this);if(execution.flow==ExecFlow::Return){result=execution.value;break;}}env=old;releaseScope(std::move(local));return result;
    }
};

namespace {
struct Literal:Expr{Value v;explicit Literal(Value x):v(std::move(x)){}Value eval(Runtime&)override{return v;}};
struct Variable:Expr{std::string n;explicit Variable(std::string x):n(std::move(x)){}Value eval(Runtime&r)override{return r.env->get(n);}};
struct ArrayExpr:Expr{std::vector<std::shared_ptr<Expr>> xs;Value eval(Runtime&r)override{auto a=std::make_shared<Value::Array>();for(auto&x:xs)a->push_back(x->eval(r));Value v;v.data=a;return v;}};
struct ObjectExpr:Expr{std::vector<std::pair<std::string,std::shared_ptr<Expr>>> xs;Value eval(Runtime&r)override{auto o=std::make_shared<Object>();for(auto&[k,x]:xs)o->fields[k]=x->eval(r);Value v;v.data=o;return v;}};
struct FunctionExpr:Expr{std::shared_ptr<Function>f;explicit FunctionExpr(std::shared_ptr<Function>x):f(std::move(x)){}Value eval(Runtime&r)override{auto instance=std::make_shared<Function>(*f);instance->closure=r.env==r.globals?std::shared_ptr<Environment>{}:r.env;Value value;value.data=instance;return value;}};
struct TemplateExpr:Expr{struct Part{std::string text;std::shared_ptr<Expr>expression;};std::vector<Part>parts;Value eval(Runtime&r)override{std::string result;for(auto&part:parts){result+=part.text;if(part.expression)result+=part.expression->eval(r).asString();}return result;}};
struct Unary:Expr{TK op;std::shared_ptr<Expr>x;Unary(TK o,std::shared_ptr<Expr>e):op(o),x(std::move(e)){}Value eval(Runtime&r)override{auto v=x->eval(r);if(op==TK::Bang)return!v.truthy();if(op==TK::Minus)return-v.asNumber();if(op==TK::Typeof)return typeOf(v);return v.asNumber();}};
struct Binary:Expr{std::shared_ptr<Expr>a,b;TK op;Binary(std::shared_ptr<Expr>x,std::shared_ptr<Expr>y,TK o):a(std::move(x)),b(std::move(y)),op(o){}Value eval(Runtime&r)override{auto l=a->eval(r);if(op==TK::AndAnd)return l.truthy()?b->eval(r):l;if(op==TK::OrOr)return l.truthy()?l:b->eval(r);auto q=b->eval(r);switch(op){case TK::Plus:if(std::holds_alternative<std::string>(l.data)||std::holds_alternative<std::string>(q.data))return l.asString()+q.asString();return l.asNumber()+q.asNumber();case TK::Minus:return l.asNumber()-q.asNumber();case TK::Star:return l.asNumber()*q.asNumber();case TK::Slash:return q.asNumber()==0?Value{}:Value(l.asNumber()/q.asNumber());case TK::Percent:return std::fmod(l.asNumber(),q.asNumber());case TK::Power:return std::pow(l.asNumber(),q.asNumber());case TK::EqualEqual:return l.asString()==q.asString();case TK::BangEqual:return l.asString()!=q.asString();case TK::StrictEqual:return strictEqual(l,q);case TK::StrictBangEqual:return!strictEqual(l,q);case TK::Less:return l.asNumber()<q.asNumber();case TK::LessEqual:return l.asNumber()<=q.asNumber();case TK::Greater:return l.asNumber()>q.asNumber();case TK::GreaterEqual:return l.asNumber()>=q.asNumber();default:return{};}}};
struct Member:Expr{std::shared_ptr<Expr>object;std::string name;Member(std::shared_ptr<Expr>o,std::string n):object(std::move(o)),name(std::move(n)){}Value eval(Runtime&r)override{return r.getMember(object->eval(r),name);}};
struct Index:Expr{std::shared_ptr<Expr>object,index;Index(std::shared_ptr<Expr>o,std::shared_ptr<Expr>i):object(std::move(o)),index(std::move(i)){}Value eval(Runtime&r)override{return r.getIndex(object->eval(r),index->eval(r));}};
struct Call:Expr{std::shared_ptr<Expr>callee;std::vector<std::shared_ptr<Expr>>args;Call(std::shared_ptr<Expr>c,std::vector<std::shared_ptr<Expr>>a):callee(std::move(c)),args(std::move(a)){}Value eval(Runtime&r)override{std::vector<Value>v;for(auto&a:args)v.push_back(a->eval(r));return r.call(callee->eval(r),v);}};
struct Conditional:Expr{std::shared_ptr<Expr>condition,yes,no;Conditional(std::shared_ptr<Expr>c,std::shared_ptr<Expr>y,std::shared_ptr<Expr>n):condition(std::move(c)),yes(std::move(y)),no(std::move(n)){}Value eval(Runtime&r)override{return condition->eval(r).truthy()?yes->eval(r):no->eval(r);}};
struct Assign:Expr{std::shared_ptr<Expr>target,value;TK op;Assign(std::shared_ptr<Expr>t,std::shared_ptr<Expr>v,TK o):target(std::move(t)),value(std::move(v)),op(o){}Value eval(Runtime&r)override{Value v=value->eval(r),old=target->eval(r);if(op==TK::PlusEqual)v=Binary(std::make_shared<Literal>(old),std::make_shared<Literal>(v),TK::Plus).eval(r);else if(op==TK::MinusEqual)v=old.asNumber()-v.asNumber();else if(op==TK::StarEqual)v=old.asNumber()*v.asNumber();else if(op==TK::SlashEqual)v=v.asNumber()==0?Value{}:Value(old.asNumber()/v.asNumber());if(auto*x=dynamic_cast<Variable*>(target.get())){if(!r.env->assign(x->n,v))r.env->define(x->n,v);}else if(auto*m=dynamic_cast<Member*>(target.get()))r.setMember(m->object->eval(r),m->name,v);else if(auto*i=dynamic_cast<Index*>(target.get()))r.setIndex(i->object->eval(r),i->index->eval(r),v);else throw std::runtime_error("Invalid CafeScript assignment target");return v;}};
struct Postfix:Expr{std::shared_ptr<Expr>target;TK op;Postfix(std::shared_ptr<Expr>t,TK o):target(std::move(t)),op(o){}Value eval(Runtime&r)override{Value old=target->eval(r);Assign a(target,std::make_shared<Literal>(1.0),op==TK::PlusPlus?TK::PlusEqual:TK::MinusEqual);a.eval(r);return old;}};
struct Prefix:Expr{std::shared_ptr<Expr>target;TK op;Prefix(std::shared_ptr<Expr>t,TK o):target(std::move(t)),op(o){}Value eval(Runtime&r)override{Assign assignment(target,std::make_shared<Literal>(1.0),op==TK::PlusPlus?TK::PlusEqual:TK::MinusEqual);return assignment.eval(r);}};

struct ExprStmt:Stmt{std::shared_ptr<Expr>x;explicit ExprStmt(std::shared_ptr<Expr>e):x(std::move(e)){}ExecResult exec(Runtime&r)override{x->eval(r);return{};}};
struct LetStmt:Stmt{std::string n;std::shared_ptr<Expr>x;bool constant;LetStmt(std::string name,std::shared_ptr<Expr>e,bool c=false):n(std::move(name)),x(std::move(e)),constant(c){}ExecResult exec(Runtime&r)override{r.env->define(n,x?x->eval(r):Value{},constant);return{};}};
struct BlockStmt:Stmt{std::vector<std::shared_ptr<Stmt>> body;ExecResult exec(Runtime&r)override{auto old=r.env;auto local=r.acquireScope(old);r.env=local;for(auto&s:body){auto q=s->exec(r);if(q.flow!=ExecFlow::Normal){r.env=old;r.releaseScope(std::move(local));return q;}}r.env=old;r.releaseScope(std::move(local));return{};}};
struct IfStmt:Stmt{std::shared_ptr<Expr>condition;std::shared_ptr<Stmt>yes,no;IfStmt(std::shared_ptr<Expr>c,std::shared_ptr<Stmt>y,std::shared_ptr<Stmt>n):condition(std::move(c)),yes(std::move(y)),no(std::move(n)){}ExecResult exec(Runtime&r)override{return condition->eval(r).truthy()?yes->exec(r):(no?no->exec(r):ExecResult{});}};
struct WhileStmt:Stmt{std::shared_ptr<Expr>condition;std::shared_ptr<Stmt>body;WhileStmt(std::shared_ptr<Expr>c,std::shared_ptr<Stmt>b):condition(std::move(c)),body(std::move(b)){}ExecResult exec(Runtime&r)override{int guard=0;while(condition->eval(r).truthy()){auto q=body->exec(r);if(q.flow==ExecFlow::Return)return q;if(q.flow==ExecFlow::Break)break;if(++guard>100000)throw std::runtime_error("CafeScript loop limit exceeded");}return{};}};
struct ForStmt:Stmt{std::shared_ptr<Stmt>init;std::shared_ptr<Expr>condition,increment;std::shared_ptr<Stmt>body;ForStmt(std::shared_ptr<Stmt>i,std::shared_ptr<Expr>c,std::shared_ptr<Expr>n,std::shared_ptr<Stmt>b):init(std::move(i)),condition(std::move(c)),increment(std::move(n)),body(std::move(b)){}ExecResult exec(Runtime&r)override{if(init)init->exec(r);int guard=0;while(!condition||condition->eval(r).truthy()){auto q=body->exec(r);if(q.flow==ExecFlow::Return)return q;if(q.flow==ExecFlow::Break)break;if(increment)increment->eval(r);if(++guard>100000)throw std::runtime_error("CafeScript loop limit exceeded");}return{};}};
struct ForOfStmt:Stmt{std::string name;std::shared_ptr<Expr>source;std::shared_ptr<Stmt>body;bool constant;ForOfStmt(std::string n,std::shared_ptr<Expr>s,std::shared_ptr<Stmt>b,bool c):name(std::move(n)),source(std::move(s)),body(std::move(b)),constant(c){}ExecResult exec(Runtime&r)override{Value iterable=source->eval(r);std::vector<Value>values;if(auto array=std::get_if<std::shared_ptr<Value::Array>>(&iterable.data))values=**array;else if(auto text=std::get_if<std::string>(&iterable.data))for(char c:*text)values.emplace_back(std::string(1,c));else return{};auto old=r.env;auto loop=r.acquireScope(old);r.env=loop;int guard=0;for(const auto&value:values){loop->define(name,value,constant);auto q=body->exec(r);if(q.flow==ExecFlow::Return){r.env=old;r.releaseScope(std::move(loop));return q;}if(q.flow==ExecFlow::Break)break;if(++guard>100000){r.env=old;throw std::runtime_error("CafeScript loop limit exceeded");}}r.env=old;r.releaseScope(std::move(loop));return{};}};
struct ReturnStmt:Stmt{std::shared_ptr<Expr>x;explicit ReturnStmt(std::shared_ptr<Expr>e):x(std::move(e)){}ExecResult exec(Runtime&r)override{return{ExecFlow::Return,x?x->eval(r):Value{}};}};
struct FlowStmt:Stmt{ExecFlow flow;explicit FlowStmt(ExecFlow f):flow(f){}ExecResult exec(Runtime&)override{return{flow,{}};}};
struct FunctionStmt:Stmt{std::string name;std::shared_ptr<Function>f;FunctionStmt(std::string n,std::shared_ptr<Function>x):name(std::move(n)),f(std::move(x)){}ExecResult exec(Runtime&r)override{auto instance=std::make_shared<Function>(*f);instance->closure=r.env==r.globals?std::shared_ptr<Environment>{}:r.env;Value v;v.data=instance;r.env->define(name,v);return{};}};
struct EventStmt:Stmt{std::string key;std::shared_ptr<Function>f;EventStmt(std::string k,std::shared_ptr<Function>x):key(std::move(k)),f(std::move(x)){}ExecResult exec(Runtime&r)override{r.events[key].push_back(f);return{};}};

class Parser {
public:
    explicit Parser(std::vector<Token> t):t_(std::move(t)){}
    std::vector<std::shared_ptr<Stmt>> program(){std::vector<std::shared_ptr<Stmt>>s;while(!check(TK::End))s.push_back(statement());return s;}
private:
    std::vector<Token>t_;std::size_t p_{0};
    bool check(TK k)const{return t_[p_].kind==k;} bool match(TK k){if(check(k)){++p_;return true;}return false;}Token take(TK k,const char*m){if(!check(k))throw std::runtime_error(std::string(m)+" on CafeScript line "+std::to_string(t_[p_].line));return t_[p_++];}
    std::vector<std::shared_ptr<Stmt>> block(){take(TK::LBrace,"Expected '{'");std::vector<std::shared_ptr<Stmt>>b;while(!check(TK::RBrace)&&!check(TK::End))b.push_back(statement());take(TK::RBrace,"Expected '}'");return b;}
    std::shared_ptr<Stmt> statement(){
        if(check(TK::Let)||check(TK::Const)||check(TK::Var)){const bool constant=check(TK::Const);++p_;return letStatement(true,constant);}
        if(match(TK::Function))return functionStatement();
        if(match(TK::If))return ifStatement();
        if(match(TK::While))return whileStatement();
        if(match(TK::For))return forStatement();
        if(match(TK::Break)){match(TK::Semicolon);return std::make_shared<FlowStmt>(ExecFlow::Break);}
        if(match(TK::Continue)){match(TK::Semicolon);return std::make_shared<FlowStmt>(ExecFlow::Continue);}
        if(match(TK::Return)){auto x=check(TK::Semicolon)?nullptr:expression();match(TK::Semicolon);return std::make_shared<ReturnStmt>(ReturnStmt{x});}
        if(match(TK::Hash)){auto id=take(TK::Identifier,"Expected element id").text;take(TK::On,"Expected 'on'");auto event=take(TK::Identifier,"Expected event name").text;auto f=std::make_shared<Function>();f->body=block();return std::make_shared<EventStmt>(EventStmt{id+":"+event,f});}
        if(match(TK::On)){auto event=take(TK::Identifier,"Expected event name").text;auto f=std::make_shared<Function>();f->body=block();return std::make_shared<EventStmt>(EventStmt{"*:"+event,f});}
        if(check(TK::LBrace)){auto b=std::make_shared<BlockStmt>();b->body=block();return b;}auto x=expression();match(TK::Semicolon);return std::make_shared<ExprStmt>(ExprStmt{x}); }
    std::shared_ptr<Stmt> letStatement(bool semi,bool constant=false){auto n=take(TK::Identifier,"Expected variable name").text;std::shared_ptr<Expr>x;if(match(TK::Equal))x=expression();else if(constant)throw std::runtime_error("const requires an initializer on CafeScript line "+std::to_string(t_[p_].line));if(semi)match(TK::Semicolon);return std::make_shared<LetStmt>(LetStmt{n,x,constant});}
    std::shared_ptr<Stmt> functionStatement(){auto n=take(TK::Identifier,"Expected function name").text;take(TK::LParen,"Expected '('");auto f=std::make_shared<Function>();if(!check(TK::RParen))do{f->params.push_back(take(TK::Identifier,"Expected parameter").text);}while(match(TK::Comma));take(TK::RParen,"Expected ')'");f->body=block();return std::make_shared<FunctionStmt>(FunctionStmt{n,f});}
    std::shared_ptr<Stmt> controlBody(){if(check(TK::LBrace)){auto body=std::make_shared<BlockStmt>();body->body=block();return body;}return statement();}
    std::shared_ptr<Stmt> ifStatement(){take(TK::LParen,"Expected '('");auto c=expression();take(TK::RParen,"Expected ')'");auto y=controlBody();std::shared_ptr<Stmt>n;if(match(TK::Else))n=check(TK::If)?statement():controlBody();return std::make_shared<IfStmt>(IfStmt{c,y,n});}
    std::shared_ptr<Stmt> whileStatement(){take(TK::LParen,"Expected '('");auto c=expression();take(TK::RParen,"Expected ')'");return std::make_shared<WhileStmt>(WhileStmt{c,statement()});}
    std::shared_ptr<Stmt> forStatement(){take(TK::LParen,"Expected '('");std::shared_ptr<Stmt>i;if(check(TK::Let)||check(TK::Const)||check(TK::Var)){const bool constant=check(TK::Const);++p_;auto name=take(TK::Identifier,"Expected variable name").text;if(match(TK::Of)){auto source=expression();take(TK::RParen,"Expected ')'");return std::make_shared<ForOfStmt>(name,source,statement(),constant);}std::shared_ptr<Expr>value;if(match(TK::Equal))value=expression();else if(constant)throw std::runtime_error("const requires an initializer on CafeScript line "+std::to_string(t_[p_].line));i=std::make_shared<LetStmt>(name,value,constant);}else if(!check(TK::Semicolon))i=std::make_shared<ExprStmt>(ExprStmt{expression()});take(TK::Semicolon,"Expected ';'");std::shared_ptr<Expr>c;if(!check(TK::Semicolon))c=expression();take(TK::Semicolon,"Expected ';'");std::shared_ptr<Expr>inc;if(!check(TK::RParen))inc=expression();take(TK::RParen,"Expected ')'");return std::make_shared<ForStmt>(ForStmt{i,c,inc,statement()});}
    std::shared_ptr<Expr> expression(){return assignment();}
    std::shared_ptr<Expr> assignment(){auto x=conditional();if(check(TK::Equal)||check(TK::PlusEqual)||check(TK::MinusEqual)||check(TK::StarEqual)||check(TK::SlashEqual)){auto op=t_[p_++].kind;auto v=assignment();return std::make_shared<Assign>(Assign{x,v,op});}return x;}
    std::shared_ptr<Expr> conditional(){auto x=logicOr();if(match(TK::Question)){auto yes=assignment();take(TK::Colon,"Expected ':' in conditional");auto no=assignment();return std::make_shared<Conditional>(x,yes,no);}return x;}
    template<class Next>std::shared_ptr<Expr>chain(Next next,std::initializer_list<TK>ops){auto x=(this->*next)();for(;;){TK op=t_[p_].kind;if(std::find(ops.begin(),ops.end(),op)==ops.end())break;++p_;x=std::make_shared<Binary>(Binary{x,(this->*next)(),op});}return x;}
    std::shared_ptr<Expr>logicOr(){return chain(&Parser::logicAnd,{TK::OrOr});}std::shared_ptr<Expr>logicAnd(){return chain(&Parser::equality,{TK::AndAnd});}std::shared_ptr<Expr>equality(){return chain(&Parser::comparison,{TK::EqualEqual,TK::BangEqual,TK::StrictEqual,TK::StrictBangEqual});}std::shared_ptr<Expr>comparison(){return chain(&Parser::term,{TK::Less,TK::LessEqual,TK::Greater,TK::GreaterEqual});}std::shared_ptr<Expr>term(){return chain(&Parser::factor,{TK::Plus,TK::Minus});}std::shared_ptr<Expr>factor(){return chain(&Parser::unary,{TK::Star,TK::Slash,TK::Percent});}
    std::shared_ptr<Expr>unary(){if(check(TK::PlusPlus)||check(TK::MinusMinus)){TK op=t_[p_++].kind;return std::make_shared<Prefix>(unary(),op);}if(check(TK::Bang)||check(TK::Minus)||check(TK::Plus)||check(TK::Typeof)){TK op=t_[p_++].kind;return std::make_shared<Unary>(Unary{op,unary()});}return power();}
    std::shared_ptr<Expr>power(){auto x=postfix();if(match(TK::Power))x=std::make_shared<Binary>(x,unary(),TK::Power);return x;}
    std::shared_ptr<Expr>postfix(){auto x=primary();for(;;){if(match(TK::Dot))x=std::make_shared<Member>(Member{x,take(TK::Identifier,"Expected property").text});else if(match(TK::LBracket)){auto index=expression();take(TK::RBracket,"Expected ']'");x=std::make_shared<Index>(x,index);}else if(match(TK::LParen)){std::vector<std::shared_ptr<Expr>>a;if(!check(TK::RParen)){do{a.push_back(expression());if(check(TK::RParen))break;}while(match(TK::Comma));}take(TK::RParen,"Expected ')'");x=std::make_shared<Call>(Call{x,a});}else if(check(TK::PlusPlus)||check(TK::MinusMinus)){TK op=t_[p_++].kind;x=std::make_shared<Postfix>(Postfix{x,op});}else break;}return x;}
    bool parenthesizedArrowAhead()const{if(!check(TK::LParen))return false;std::size_t i=p_+1;if(t_[i].kind==TK::RParen)return t_[i+1].kind==TK::Arrow;for(;;){if(t_[i].kind!=TK::Identifier)return false;++i;if(t_[i].kind==TK::RParen)return t_[i+1].kind==TK::Arrow;if(t_[i].kind!=TK::Comma)return false;++i;}}
    std::shared_ptr<Expr> arrowBody(std::vector<std::string>params){auto f=std::make_shared<Function>();f->params=std::move(params);if(check(TK::LBrace))f->body=block();else f->body.push_back(std::make_shared<ReturnStmt>(expression()));return std::make_shared<FunctionExpr>(f);}
    std::shared_ptr<Expr> functionExpression(){if(check(TK::Identifier))++p_;take(TK::LParen,"Expected '('");auto f=std::make_shared<Function>();if(!check(TK::RParen))do{f->params.push_back(take(TK::Identifier,"Expected parameter").text);}while(match(TK::Comma));take(TK::RParen,"Expected ')'");f->body=block();return std::make_shared<FunctionExpr>(f);}
    std::shared_ptr<Expr> templateExpression(std::string source){auto result=std::make_shared<TemplateExpr>();std::size_t cursor=0;while(cursor<source.size()){auto open=source.find("${",cursor);if(open==std::string::npos){result->parts.push_back({source.substr(cursor),nullptr});break;}result->parts.push_back({source.substr(cursor,open-cursor),nullptr});std::size_t close=open+2;int depth=1;char quote=0;for(;close<source.size()&&depth>0;++close){char c=source[close];if(quote){if(c=='\\')++close;else if(c==quote)quote=0;continue;}if(c=='\''||c=='"'||c=='`'){quote=c;continue;}if(c=='{')++depth;else if(c=='}')--depth;}if(depth!=0)throw std::runtime_error("Unclosed template expression");const auto expressionSource=source.substr(open+2,close-open-3);Parser nested(Lexer(expressionSource).scan());auto value=nested.expression();if(!nested.check(TK::End))throw std::runtime_error("Invalid template expression");result->parts.push_back({{},value});cursor=close;}if(source.empty())result->parts.push_back({{},nullptr});return result;}
    std::shared_ptr<Expr>primary(){if(parenthesizedArrowAhead()){take(TK::LParen,"Expected '('");std::vector<std::string>params;if(!check(TK::RParen))do{params.push_back(take(TK::Identifier,"Expected parameter").text);}while(match(TK::Comma));take(TK::RParen,"Expected ')'");take(TK::Arrow,"Expected '=>'");return arrowBody(std::move(params));}if(check(TK::Identifier)&&p_+1<t_.size()&&t_[p_+1].kind==TK::Arrow){std::vector<std::string>params{t_[p_++].text};++p_;return arrowBody(std::move(params));}if(match(TK::True))return std::make_shared<Literal>(true);if(match(TK::False))return std::make_shared<Literal>(false);if(match(TK::Null))return std::make_shared<Literal>(Value(nullptr));if(match(TK::Undefined))return std::make_shared<Literal>(Value{});if(check(TK::Number)){auto n=t_[p_++].number;return std::make_shared<Literal>(n);}if(check(TK::String)){auto s=t_[p_++].text;return std::make_shared<Literal>(s);}if(check(TK::Template)){auto s=t_[p_++].text;return templateExpression(std::move(s));}if(match(TK::Function))return functionExpression();if(check(TK::Identifier))return std::make_shared<Variable>(t_[p_++].text);if(match(TK::LParen)){auto x=expression();take(TK::RParen,"Expected ')'");return x;}if(match(TK::LBracket)){auto a=std::make_shared<ArrayExpr>();if(!check(TK::RBracket)){do{a->xs.push_back(expression());if(check(TK::RBracket))break;}while(match(TK::Comma));}take(TK::RBracket,"Expected ']'");return a;}if(match(TK::LBrace)){auto o=std::make_shared<ObjectExpr>();if(!check(TK::RBrace)){do{std::string k=(check(TK::Identifier)||check(TK::String))?t_[p_++].text:take(TK::Identifier,"Expected key").text;std::shared_ptr<Expr>value;if(match(TK::Colon))value=expression();else value=std::make_shared<Variable>(k);o->xs.push_back({k,value});if(check(TK::RBrace))break;}while(match(TK::Comma));}take(TK::RBrace,"Expected '}'");return o;}throw std::runtime_error("Expected CafeScript expression on line "+std::to_string(t_[p_].line));}
};

Value object(std::initializer_list<std::pair<const std::string,Value>> fields){auto o=std::make_shared<Object>();for(auto&[k,v]:fields)o->fields[k]=v;Value result;result.data=o;return result;}
Value native(std::function<Value(const std::vector<Value>&)> callback){return makeNative(std::move(callback));}

Value buttonFields() {
    return object({
        {"A", false}, {"B", false}, {"X", false}, {"Y", false},
        {"L", false}, {"R", false}, {"ZL", false}, {"ZR", false},
        {"plus", false}, {"minus", false},
        {"up", false}, {"down", false}, {"left", false}, {"right", false}
    });
}

Value keyboardFields() {
    auto keys = std::make_shared<Object>();
    for (char key = 'a'; key <= 'z'; ++key) keys->fields[std::string(1, key)] = false;
    for (const char* name : {"left", "right", "up", "down", "space", "enter",
                             "escape", "backspace"}) keys->fields[name] = false;
    Value value; value.data = keys; return value;
}
}

struct Interpreter::Impl {
    struct Timer {
        int id{0};
        Value callback;
        double remaining{0};
        double interval{0};
        bool repeating{false};
    };
    Runtime runtime; std::vector<std::shared_ptr<Stmt>> program;
    std::vector<Timer> timers;
    int nextTimerId{1};
    Impl(SceneGraph&g,InputSystem&i):runtime(g,i){
        auto openOn = [this](const std::string& id, Display display) {
            const bool opened = runtime.graph.openScene(id, display);
            if (opened && runtime.sceneOpen && *runtime.sceneOpen) (*runtime.sceneOpen)(id);
            return opened;
        };
        auto log = native([](const auto&a){for(std::size_t i=0;i<a.size();++i)std::cout<<(i?" ":"")<<a[i].asString();std::cout<<'\n';return Value{};});
        runtime.globals->define("print",log);
        runtime.globals->define("console",object({{"log",log}}));
        runtime.globals->define("Math",object({
            {"PI",3.14159265358979323846},
            {"abs",native([](const auto&a)->Value{return a.empty()?Value{}:Value(std::abs(a[0].asNumber()));})},
            {"floor",native([](const auto&a)->Value{return a.empty()?Value{}:Value(std::floor(a[0].asNumber()));})},
            {"ceil",native([](const auto&a)->Value{return a.empty()?Value{}:Value(std::ceil(a[0].asNumber()));})},
            {"round",native([](const auto&a)->Value{return a.empty()?Value{}:Value(std::round(a[0].asNumber()));})},
            {"sqrt",native([](const auto&a)->Value{return a.empty()?Value{}:Value(std::sqrt(a[0].asNumber()));})},
            {"sin",native([](const auto&a)->Value{return a.empty()?Value{}:Value(std::sin(a[0].asNumber()));})},
            {"cos",native([](const auto&a)->Value{return a.empty()?Value{}:Value(std::cos(a[0].asNumber()));})},
            {"tan",native([](const auto&a)->Value{return a.empty()?Value{}:Value(std::tan(a[0].asNumber()));})},
            {"atan2",native([](const auto&a)->Value{return a.size()<2?Value{}:Value(std::atan2(a[0].asNumber(),a[1].asNumber()));})},
            {"sign",native([](const auto&a)->Value{if(a.empty())return{};const double x=a[0].asNumber();return x>0?Value(1.0):x<0?Value(-1.0):Value(0.0);})},
            {"trunc",native([](const auto&a)->Value{return a.empty()?Value{}:Value(std::trunc(a[0].asNumber()));})},
            {"log",native([](const auto&a)->Value{return a.empty()?Value{}:Value(std::log(a[0].asNumber()));})},
            {"exp",native([](const auto&a)->Value{return a.empty()?Value{}:Value(std::exp(a[0].asNumber()));})},
            {"random",native([state=std::uint32_t{0x51A7E5u}](const auto&)mutable->Value{state=state*1664525u+1013904223u;return Value(static_cast<double>((state>>8)&0xFFFFFFu)/16777216.0);})},
            {"pow",native([](const auto&a)->Value{return a.size()<2?Value{}:Value(std::pow(a[0].asNumber(),a[1].asNumber()));})},
            {"min",native([](const auto&a)->Value{if(a.empty())return{};double value=a[0].asNumber();for(std::size_t i=1;i<a.size();++i)value=std::min(value,a[i].asNumber());return value;})},
            {"max",native([](const auto&a)->Value{if(a.empty())return{};double value=a[0].asNumber();for(std::size_t i=1;i<a.size();++i)value=std::max(value,a[i].asNumber());return value;})},
            {"clamp",native([](const auto&a)->Value{return a.size()<3?Value{}:Value(std::clamp(a[0].asNumber(),a[1].asNumber(),a[2].asNumber()));})}
        }));
        runtime.globals->define("Array",object({{"isArray",native([](const auto&a){return Value(!a.empty()&&std::holds_alternative<std::shared_ptr<Value::Array>>(a[0].data));})}}));
        runtime.globals->define("Object",object({
            {"keys",native([](const auto&a){auto out=std::make_shared<Value::Array>();if(!a.empty())if(auto o=std::get_if<std::shared_ptr<Object>>(&a[0].data))for(const auto&[key,value]:(*o)->fields)out->emplace_back(key);Value result;result.data=out;return result;})},
            {"values",native([](const auto&a){auto out=std::make_shared<Value::Array>();if(!a.empty())if(auto o=std::get_if<std::shared_ptr<Object>>(&a[0].data))for(const auto&[key,value]:(*o)->fields)out->push_back(value);Value result;result.data=out;return result;})},
            {"entries",native([](const auto&a){auto out=std::make_shared<Value::Array>();if(!a.empty())if(auto o=std::get_if<std::shared_ptr<Object>>(&a[0].data))for(const auto&[key,value]:(*o)->fields){auto pair=std::make_shared<Value::Array>();pair->emplace_back(key);pair->push_back(value);Value entry;entry.data=pair;out->push_back(entry);}Value result;result.data=out;return result;})},
            {"assign",native([](const auto&a){if(a.empty()||!std::holds_alternative<std::shared_ptr<Object>>(a[0].data))return Value{};auto target=std::get<std::shared_ptr<Object>>(a[0].data);for(std::size_t i=1;i<a.size();++i)if(auto source=std::get_if<std::shared_ptr<Object>>(&a[i].data))for(const auto&[key,value]:(*source)->fields)target->fields[key]=value;return a[0];})}
        }));
        runtime.globals->define("Number",native([](const auto&a)->Value{return a.empty()?Value(0.0):Value(a[0].asNumber());}));
        runtime.globals->define("String",native([](const auto&a)->Value{return a.empty()?Value(std::string{}):Value(a[0].asString());}));
        runtime.globals->define("Boolean",native([](const auto&a){return Value(!a.empty()&&a[0].truthy());}));
        runtime.globals->define("undefined",Value{});
        runtime.globals->define("Time",object({{"delta",0.0},{"seconds",0.0}}));
        auto schedule = [this](const std::vector<Value>& a, bool repeat) -> Value {
            if (a.empty() || !std::holds_alternative<std::shared_ptr<Function>>(a[0].data))
                return 0.0;
            const double delay = std::max(0.0, a.size() > 1 ? a[1].asNumber() / 1000.0 : 0.0);
            const int id = nextTimerId++;
            timers.push_back({id, a[0], delay, std::max(0.001, delay), repeat});
            return static_cast<double>(id);
        };
        runtime.globals->define("setTimeout", native([schedule](const auto& a) {
            return schedule(a, false);
        }));
        runtime.globals->define("setInterval", native([schedule](const auto& a) {
            return schedule(a, true);
        }));
        auto clearTimer = native([this](const auto& a) -> Value {
            if (!a.empty()) {
                const int id = static_cast<int>(a[0].asNumber());
                std::erase_if(timers, [id](const Timer& timer) { return timer.id == id; });
            }
            return {};
        });
        runtime.globals->define("clearTimeout", clearTimer);
        runtime.globals->define("clearInterval", clearTimer);
        auto pad = buttonFields();
        runtime.setMember(pad, "pressed", buttonFields());
        runtime.setMember(pad, "released", buttonFields());
        runtime.setMember(pad, "leftX", 0.0);
        runtime.setMember(pad, "leftY", 0.0);
        runtime.setMember(pad, "rightX", 0.0);
        runtime.setMember(pad, "rightY", 0.0);
        runtime.globals->define("GamePad", pad);
        auto remote = object({
            {"connected", false}, {"valid", false}, {"x", 0.0}, {"y", 0.0},
            {"A", false}, {"B", false}, {"one", false}, {"two", false},
            {"plus", false}, {"minus", false}, {"home", false},
            {"up", false}, {"down", false}, {"left", false}, {"right", false},
            {"C", false}, {"Z", false},
            {"accX", 0.0}, {"accY", 0.0}, {"accZ", 0.0}
        });
        runtime.setMember(remote, "pressed", object({
            {"A", false}, {"B", false}, {"one", false}, {"two", false},
            {"plus", false}, {"minus", false}, {"home", false},
            {"up", false}, {"down", false}, {"left", false}, {"right", false},
            {"C", false}, {"Z", false}
        }));
        runtime.setMember(remote, "released", object({
            {"A", false}, {"B", false}, {"one", false}, {"two", false},
            {"plus", false}, {"minus", false}, {"home", false},
            {"up", false}, {"down", false}, {"left", false}, {"right", false},
            {"C", false}, {"Z", false}
        }));
        runtime.setMember(remote, "nunchuk", object({
            {"x", 0.0}, {"y", 0.0}, {"C", false}, {"Z", false}
        }));
        runtime.globals->define("WiiRemote", remote);
        auto keyboard = keyboardFields();
        runtime.setMember(keyboard, "pressed", keyboardFields());
        runtime.setMember(keyboard, "released", keyboardFields());
        runtime.globals->define("Keyboard", keyboard);
        auto mouse = object({{"x",0.0},{"y",0.0},{"left",false},{"right",false},
                             {"display","gamepad"}});
        runtime.setMember(mouse, "pressed", object({{"left",false},{"right",false}}));
        runtime.setMember(mouse, "released", object({{"left",false},{"right",false}}));
        runtime.globals->define("Mouse", mouse);
        runtime.globals->define("Scene", object({
            {"open", native([openOn](const auto& a) {
                if (a.empty()) return Value(false);
                Display display = a.size() >= 2 ? cafe::parseDisplay(a[1].asString()) : Display::Both;
                return Value(openOn(a[0].asString(), display));
            })},
            {"tv", object({{"open", native([openOn](const auto& a) {
                if (a.empty()) return Value(false);
                return Value(openOn(a[0].asString(), Display::Tv));
            })}, {"id", std::string{}}})},
            {"gamepad", object({{"open", native([openOn](const auto& a) {
                if (a.empty()) return Value(false);
                return Value(openOn(a[0].asString(), Display::GamePad));
            })}, {"id", std::string{}}})}
        }));
        runtime.globals->define("Audio", object({
            {"play", native([this](const auto& a) -> Value {
                if (a.empty() || !runtime.audioPlay || !*runtime.audioPlay) return 0.0;
                bool loop = false; double volume = 1.0;
                if (a.size() > 1) {
                    if (std::holds_alternative<std::shared_ptr<Object>>(a[1].data)) {
                        loop = runtime.getMember(a[1], "loop").truthy();
                        Value option = runtime.getMember(a[1], "volume");
                        if (!std::holds_alternative<std::monostate>(option.data)) volume = option.asNumber();
                    } else loop = a[1].truthy();
                }
                return static_cast<double>((*runtime.audioPlay)(a[0].asString(), loop, volume));
            })},
            {"stop", native([this](const auto& a) -> Value {
                if (runtime.audioStop && *runtime.audioStop) (*runtime.audioStop)(a.empty() ? 0 : static_cast<int>(a[0].asNumber()));
                return {};
            })},
            {"pause", native([this](const auto& a) -> Value {
                if (!a.empty() && runtime.audioControl && *runtime.audioControl)
                    (*runtime.audioControl)(static_cast<int>(a[0].asNumber()), "pause", 0);
                return {};
            })},
            {"resume", native([this](const auto& a) -> Value {
                if (!a.empty() && runtime.audioControl && *runtime.audioControl)
                    (*runtime.audioControl)(static_cast<int>(a[0].asNumber()), "resume", 0);
                return {};
            })},
            {"setVolume", native([this](const auto& a) -> Value {
                if (a.size() >= 2 && runtime.audioControl && *runtime.audioControl)
                    (*runtime.audioControl)(static_cast<int>(a[0].asNumber()), "volume", a[1].asNumber());
                return {};
            })},
            {"setPitch", native([this](const auto& a) -> Value {
                if (a.size() >= 2 && runtime.audioControl && *runtime.audioControl)
                    (*runtime.audioControl)(static_cast<int>(a[0].asNumber()), "pitch", a[1].asNumber());
                return {};
            })},
            {"isPlaying", native([this](const auto& a) -> Value {
                return !a.empty() && runtime.audioState && *runtime.audioState &&
                    (*runtime.audioState)(static_cast<int>(a[0].asNumber()));
            })}
        }));
        runtime.globals->define("Storage", object({
            {"read", native([this](const auto& a) -> Value {
                if (a.empty() || !runtime.storageRead || !*runtime.storageRead)
                    return a.size() > 1 ? a[1] : Value(std::string{});
                const std::string value = (*runtime.storageRead)(a[0].asString());
                return value.empty() && a.size() > 1 ? a[1] : Value(value);
            })},
            {"write", native([this](const auto& a) -> Value {
                return a.size() >= 2 && runtime.storageWrite && *runtime.storageWrite &&
                    (*runtime.storageWrite)(a[0].asString(), a[1].asString());
            })},
            {"exists", native([this](const auto& a) -> Value {
                return !a.empty() && runtime.storageExists && *runtime.storageExists &&
                    (*runtime.storageExists)(a[0].asString());
            })},
            {"remove", native([this](const auto& a) -> Value {
                return !a.empty() && runtime.storageRemove && *runtime.storageRemove &&
                    (*runtime.storageRemove)(a[0].asString());
            })}
        }));
        runtime.globals->define("Collision", object({
            {"overlaps", native([this](const auto& a) -> Value {
                if (a.size() < 2) return false;
                auto resolve = [&](const Value& value) -> Node* {
                    if (auto n = std::get_if<Node*>(&value.data)) return *n;
                    return runtime.graph.find(value.asString());
                };
                Node* left = resolve(a[0]); Node* right = resolve(a[1]);
                return left && right && SceneGraph::overlaps(*left, *right);
            })},
            {"pointIn", native([this](const auto& a) -> Value {
                if (a.size() < 3) return false;
                Node* node = nullptr;
                if (auto n = std::get_if<Node*>(&a[0].data)) node = *n;
                else node = runtime.graph.find(a[0].asString());
                return node && node->bounds().contains(a[1].asNumber(), a[2].asNumber());
            })},
            {"circles", native([](const auto& a) -> Value {
                if (a.size() < 6) return false;
                const double dx = a[3].asNumber() - a[0].asNumber();
                const double dy = a[4].asNumber() - a[1].asNumber();
                const double radius = a[2].asNumber() + a[5].asNumber();
                return dx * dx + dy * dy <= radius * radius;
            })},
            {"alpha", native([this](const auto& a) -> Value {
                if (a.size() < 3 || !runtime.alphaHit || !*runtime.alphaHit) return false;
                return (*runtime.alphaHit)(a[0].asString(), a[1].asNumber(), a[2].asNumber(),
                    a.size() > 3 ? a[3].asNumber() : 0.01);
            })}
        }));
        runtime.globals->define("Mask",object({
            {"edgePoints",native([this](const auto& a) -> Value {
                auto points=std::make_shared<Value::Array>();
                if (a.empty() || !runtime.maskEdges || !*runtime.maskEdges) {
                    Value empty;empty.data=points;return empty;
                }
                const double requested=a.size()>1?a[1].asNumber():16.0;
                const int spacing=std::isfinite(requested)
                    ?static_cast<int>(std::clamp(requested,4.0,128.0)):16;
                const double requestedThreshold=a.size()>2?a[2].asNumber():0.5;
                const double threshold=std::isfinite(requestedThreshold)
                    ?std::clamp(requestedThreshold,0.0,1.0):0.5;
                for (const MaskEdgePoint& p:(*runtime.maskEdges)(a[0].asString(),
                                                                  spacing,threshold))
                    points->push_back(object({{"x",p.x},{"y",p.y},
                                              {"nx",p.nx},{"ny",p.ny}}));
                Value result;result.data=points;return result;
            })}
        }));
        runtime.globals->define("emitParticles", native([](const auto& a) -> Value {
            if (a.empty()) return 0.0;
            Node* node = nullptr;
            if (auto n = std::get_if<Node*>(&a[0].data)) node = *n;
            if (!node || node->type != NodeType::ParticleEmitter) return 0.0;
            const std::size_t amount = static_cast<std::size_t>(std::max(0.0, a.size() > 1 ? a[1].asNumber() : 1.0));
            node->emitter.pending = std::min(node->emitter.maximum, node->emitter.pending + amount);
            return static_cast<double>(amount);
        }));
        runtime.globals->define("setSprites", native([this](const auto& a) -> Value {
            if (a.size() < 2) return 0.0;
            Node* batch = nullptr;
            if (auto node = std::get_if<Node*>(&a[0].data)) batch = *node;
            else batch = runtime.graph.find(a[0].asString());
            auto instances = std::get_if<std::shared_ptr<Value::Array>>(&a[1].data);
            if (!batch || batch->type != NodeType::SpriteBatch || !instances) return 0.0;
            batch->batch.sprites.clear();
            batch->batch.sprites.reserve(std::min(batch->batch.maximum, (*instances)->size()));
            for (const Value& value : **instances) {
                if (batch->batch.sprites.size() >= batch->batch.maximum) break;
                auto fields = std::get_if<std::shared_ptr<Object>>(&value.data);
                if (!fields) continue;
                SpriteInstance item;
                auto option = [&](const char* key, Value fallback) {
                    auto found = (*fields)->fields.find(key);
                    return found == (*fields)->fields.end() ? fallback : found->second;
                };
                item.x = option("x", 0.0).asNumber();
                item.y = option("y", 0.0).asNumber();
                item.width = option("width", 0.0).asNumber();
                item.height = option("height", 0.0).asNumber();
                item.rotation = option("rotation", 0.0).asNumber();
                item.scaleX = option("scaleX", 1.0).asNumber();
                item.scaleY = option("scaleY", 1.0).asNumber();
                item.opacity = option("opacity", 1.0).asNumber();
                item.visible = option("visible", true).truthy();
                if (auto color = Color::parse(option("tint", "#ffffffff").asString()))
                    item.tint = *color;
                batch->batch.sprites.push_back(item);
            }
            ++batch->batch.revision;
            return static_cast<double>(batch->batch.sprites.size());
        }));
        runtime.globals->define("spawn", native([this](const auto& a) -> Value {
            if (a.empty()) return {};
            std::string parent;
            double x = 0, y = 0;
            if (a.size() == 3) { x = a[1].asNumber(); y = a[2].asNumber(); }
            else if (a.size() >= 4) {
                if (auto n = std::get_if<Node*>(&a[1].data); n && *n) parent = (*n)->id;
                else parent = a[1].asString();
                x = a[2].asNumber(); y = a[3].asNumber();
            } else if (a.size() == 2) {
                if (auto n = std::get_if<Node*>(&a[1].data); n && *n) parent = (*n)->id;
                else parent = a[1].asString();
            }
            Node* node = runtime.graph.spawn(a[0].asString(), parent, x, y);
            if (!node) return {};
            if (runtime.spawnHook && *runtime.spawnHook) (*runtime.spawnHook)(node);
            refreshNodes();
            return node;
        }));
        runtime.globals->define("destroy", native([this](const auto& a) -> Value {
            if (a.empty()) return false;
            std::string id;
            if (auto n = std::get_if<Node*>(&a[0].data); n && *n) id = (*n)->id;
            else id = a[0].asString();
            if (id.empty()) return false;
            const bool ok = runtime.graph.destroy(id);
            if (ok) runtime.globals->define(id, Value{});
            refreshNodes();
            return ok;
        }));
        runtime.globals->define("overlaps", native([this](const auto& a) -> Value {
            if (a.size() < 2) return false;
            auto resolve = [&](const Value& value) -> Node* {
                if (auto n = std::get_if<Node*>(&value.data)) return *n;
                return runtime.graph.find(value.asString());
            };
            Node* left = resolve(a[0]);
            Node* right = resolve(a[1]);
            return left && right && SceneGraph::overlaps(*left, *right);
        }));
    }
    void refreshNodes(){std::vector<Node*>stack{&runtime.graph.root()};while(!stack.empty()){Node*n=stack.back();stack.pop_back();if(!n->id.empty())runtime.globals->define(n->id,n);for(auto&c:n->children)stack.push_back(c.get());}}
    void advanceTimers(double dt) {
        for (Timer& timer : timers) timer.remaining -= dt;
        std::vector<int> due;
        for (const Timer& timer : timers) if (timer.remaining <= 0) due.push_back(timer.id);
        for (const int id : due) {
            auto found = std::find_if(timers.begin(), timers.end(),
                [id](const Timer& timer) { return timer.id == id; });
            if (found == timers.end()) continue;
            const Value callback = found->callback;
            if (found->repeating) found->remaining += found->interval;
            else timers.erase(found);
            runtime.call(callback, {});
        }
    }
    void callName(std::string_view n,const std::vector<Value>&args={}){auto v=runtime.globals->get(std::string(n));runtime.call(v,args);}
    void fire(const std::string&key){auto it=runtime.events.find(key);if(it!=runtime.events.end())for(auto&f:it->second){Value v;v.data=f;runtime.call(v,{});}}
    void syncButton(const Value& pad, const char* name, const ButtonState& button) {
        runtime.setMember(pad, name, button.down);
        runtime.setMember(runtime.getMember(pad, "pressed"), name, button.pressed);
        runtime.setMember(runtime.getMember(pad, "released"), name, button.released);
    }
    void syncInput(double dt) {
        auto time = runtime.globals->get("Time");
        runtime.setMember(time, "delta", dt);
        runtime.setMember(time, "seconds", runtime.getMember(time, "seconds").asNumber() + dt);
        auto gp = runtime.globals->get("GamePad");
        const auto& pad = runtime.input.gamePad;
        syncButton(gp, "A", pad.A); syncButton(gp, "B", pad.B);
        syncButton(gp, "X", pad.X); syncButton(gp, "Y", pad.Y);
        syncButton(gp, "L", pad.L); syncButton(gp, "R", pad.R);
        syncButton(gp, "ZL", pad.ZL); syncButton(gp, "ZR", pad.ZR);
        syncButton(gp, "plus", pad.plus); syncButton(gp, "minus", pad.minus);
        syncButton(gp, "up", pad.up); syncButton(gp, "down", pad.down);
        syncButton(gp, "left", pad.left); syncButton(gp, "right", pad.right);
        runtime.setMember(gp, "leftX", pad.leftStick.x);
        runtime.setMember(gp, "leftY", pad.leftStick.y);
        runtime.setMember(gp, "rightX", pad.rightStick.x);
        runtime.setMember(gp, "rightY", pad.rightStick.y);
        auto remote = runtime.globals->get("WiiRemote");
        const auto& wr = runtime.input.wiiRemote;
        runtime.setMember(remote, "connected", wr.connected);
        runtime.setMember(remote, "valid", wr.pointerValid);
        runtime.setMember(remote, "x", wr.x);
        runtime.setMember(remote, "y", wr.y);
        syncButton(remote, "A", wr.A); syncButton(remote, "B", wr.B);
        syncButton(remote, "one", wr.one); syncButton(remote, "two", wr.two);
        syncButton(remote, "plus", wr.plus); syncButton(remote, "minus", wr.minus);
        syncButton(remote, "home", wr.home);
        syncButton(remote, "up", wr.up); syncButton(remote, "down", wr.down);
        syncButton(remote, "left", wr.left); syncButton(remote, "right", wr.right);
        syncButton(remote, "C", wr.nunchuk.C); syncButton(remote, "Z", wr.nunchuk.Z);
        runtime.setMember(remote, "accX", wr.accelerometer.x);
        runtime.setMember(remote, "accY", wr.accelerometer.y);
        runtime.setMember(remote, "accZ", wr.accelerometer.z);
        auto nunchuk = runtime.getMember(remote, "nunchuk");
        runtime.setMember(nunchuk, "x", wr.nunchuk.stick.x);
        runtime.setMember(nunchuk, "y", wr.nunchuk.stick.y);
        runtime.setMember(nunchuk, "C", wr.nunchuk.C.down);
        runtime.setMember(nunchuk, "Z", wr.nunchuk.Z.down);
        auto keyboard = runtime.globals->get("Keyboard");
        auto syncKey = [&](const char* name, std::size_t index) {
            syncButton(keyboard, name, runtime.input.keyboard.keys[index]);
        };
        for (char key = 'a'; key <= 'z'; ++key) {
            const std::string name(1, key);
            syncButton(keyboard, name.c_str(),
                       runtime.input.keyboard.keys[static_cast<unsigned char>(key)]);
        }
        syncKey("backspace", static_cast<std::size_t>(KeyCode::Backspace));
        syncKey("enter", static_cast<std::size_t>(KeyCode::Enter));
        syncKey("escape", static_cast<std::size_t>(KeyCode::Escape));
        syncKey("space", static_cast<std::size_t>(KeyCode::Space));
        syncKey("left", static_cast<std::size_t>(KeyCode::Left));
        syncKey("right", static_cast<std::size_t>(KeyCode::Right));
        syncKey("up", static_cast<std::size_t>(KeyCode::Up));
        syncKey("down", static_cast<std::size_t>(KeyCode::Down));
        auto mouse = runtime.globals->get("Mouse");
        runtime.setMember(mouse, "x", runtime.input.mouse.x);
        runtime.setMember(mouse, "y", runtime.input.mouse.y);
        runtime.setMember(mouse, "left", runtime.input.mouse.left.down);
        runtime.setMember(mouse, "right", runtime.input.mouse.right.down);
        runtime.setMember(mouse, "display", runtime.input.pointerDisplay == Display::Tv ? "tv" : "gamepad");
        runtime.setMember(runtime.getMember(mouse, "pressed"), "left", runtime.input.mouse.left.pressed);
        runtime.setMember(runtime.getMember(mouse, "pressed"), "right", runtime.input.mouse.right.pressed);
        runtime.setMember(runtime.getMember(mouse, "released"), "left", runtime.input.mouse.left.released);
        runtime.setMember(runtime.getMember(mouse, "released"), "right", runtime.input.mouse.right.released);
        auto scene = runtime.globals->get("Scene");
        runtime.setMember(runtime.getMember(scene, "tv"), "id", runtime.graph.tvScene() ? runtime.graph.tvScene()->id : std::string{});
        runtime.setMember(runtime.getMember(scene, "gamepad"), "id", runtime.graph.gamePadScene() ? runtime.graph.gamePadScene()->id : std::string{});
    }
};

Interpreter::Interpreter(SceneGraph& graph, InputSystem& input):impl_(std::make_unique<Impl>(graph,input)){
    impl_->runtime.sceneOpen=&sceneOpen_;
    impl_->runtime.spawnHook=&spawnHook_;
    impl_->runtime.audioPlay=&audioPlay_;
    impl_->runtime.audioStop=&audioStop_;
    impl_->runtime.audioControl=&audioControl_;
    impl_->runtime.audioState=&audioState_;
    impl_->runtime.storageRead=&storageRead_;
    impl_->runtime.storageWrite=&storageWrite_;
    impl_->runtime.storageExists=&storageExists_;
    impl_->runtime.storageRemove=&storageRemove_;
    impl_->runtime.alphaHit=&alphaHit_;
    impl_->runtime.maskEdges=&maskEdges_;
}
Interpreter::~Interpreter() = default;
void Interpreter::setScreenBounds(Rect tv, Rect gamePad) {
    const double left=std::min(tv.x,gamePad.x);
    const double right=std::max(tv.x+tv.width,gamePad.x+gamePad.width);
    Value screen=impl_->runtime.globals->get("Screen");
    if (!std::holds_alternative<std::shared_ptr<Object>>(screen.data)) {
        screen=object({});
        impl_->runtime.globals->define("Screen",screen);
    }
    auto& fields=std::get<std::shared_ptr<Object>>(screen.data)->fields;
    fields["left"]=left;
    fields["right"]=right;
    fields["width"]=right-left;
    fields["height"]=std::max(tv.height,gamePad.height);
    fields["tvLeft"]=tv.x;
    fields["tvWidth"]=tv.width;
    fields["gamePadLeft"]=gamePad.x;
    fields["gamePadWidth"]=gamePad.width;
}
void Interpreter::load(std::string_view source){Lexer lexer(source);Parser parser(lexer.scan());impl_->program=parser.program();impl_->refreshNodes();for(auto&s:impl_->program)s->exec(impl_->runtime);}
void Interpreter::runLoad(){impl_->callName("load");impl_->fire("*:load");}
void Interpreter::update(double dt){impl_->syncInput(dt);impl_->advanceTimers(dt);impl_->callName("update",{dt});impl_->fire("*:update");}
void Interpreter::emit(std::string_view id,std::string_view event){impl_->fire(std::string(id)+":"+std::string(event));}
Value Interpreter::global(std::string_view name)const{return impl_->runtime.globals->get(std::string(name));}

} // namespace cafe::script
