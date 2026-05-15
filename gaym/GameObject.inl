// GameObject.inl  — 템플릿 '정의'만 둔다
#pragma once
#include <type_traits>
#include <typeinfo>
#include <utility>

template<typename T>
T* GameObject::GetComponent()
{
	static_assert(std::is_base_of_v<Component, T>, "T must derive from Component");
	// typeid 비교 — dynamic_cast 대비 수십배 빠름. 단 Component 파생 클래스간 상속이 없을 때만 안전.
	//   현재 프로젝트의 모든 컴포넌트는 Component 를 직접 상속하므로 OK.
	for (auto& c : m_vComponents)
	{
		if (typeid(*c) == typeid(T))
			return static_cast<T*>(c.get());
	}
	return nullptr;
}

template<typename T, typename... TArgs>
T* GameObject::AddComponent(TArgs&&... args)
{
	static_assert(std::is_base_of_v<Component, T>, "T must derive from Component");

	// 모든 컴포넌트는 (GameObject*, ...) 생성자를 가진다는 가정
	auto up = std::make_unique<T>(this, std::forward<TArgs>(args)...);
	T* raw = up.get();
	m_vComponents.emplace_back(std::move(up));

	// Transform 캐시
	if constexpr (std::is_same_v<T, TransformComponent>)
	{
		m_pTransform = raw;
	}
	return raw;
}