shared_ptr 的赋值运算符
shared_ptr<A> a = make_shared<A>(); shared_ptr<A> b = make_shared<B>()；
赋值场景	     调用函数	                    语义	                 引用计数变化
a = b;	        赋值运算符（const shared_ptr&）	放弃 a 资源，共享 b 资源  a原资源计数-1（若有），a==b共享资源计数+1
a = nullptr;	空指针赋值运算符（nullptr_t）	 放弃 a 持有的资源所有权   a原资源计数-1（若有），a==nullptr