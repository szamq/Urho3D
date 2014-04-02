//
// Copyright (c) 2008-2014 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "Precompiled.h"
#include "Animatable.h"
#include "AttributeAnimation.h"
#include "AttributeAnimationInstance.h"
#include "Context.h"
#include "Log.h"
#include "ObjectAnimation.h"
#include "ResourceCache.h"
#include "XMLElement.h"

#include "DebugNew.h"

namespace Urho3D
{

Animatable::Animatable(Context* context) :
    Serializable(context),
    animationEnabled_(true)
{
}

Animatable::~Animatable()
{
}

void Animatable::RegisterObject(Context* context)
{
    ACCESSOR_ATTRIBUTE(Animatable, VAR_RESOURCEREF, "Object Animation", GetObjectAnimationAttr, SetObjectAnimationAttr, ResourceRef, ResourceRef(ObjectAnimation::GetTypeStatic()), AM_DEFAULT);
}

bool Animatable::Load(Deserializer& source, bool setInstanceDefault)
{
    if (!Serializable::Load(source, setInstanceDefault))
        return false;

    bool hasObjectAnimation = source.ReadBool();
    if (hasObjectAnimation)
    {
        SharedPtr<ObjectAnimation> objectAnimation(new ObjectAnimation(context_));
        if (!objectAnimation->Load(source))
            return false;
        SetObjectAnimation(objectAnimation);
    }

    unsigned count = source.ReadUInt();
    for (unsigned i = 0; i < count; ++i)
    {
        String name = source.ReadString();
        SharedPtr<AttributeAnimation> attributeAnimation(new AttributeAnimation(context_));
        if (!attributeAnimation->Load(source))
            return false;

        SetAttributeAnimation(name, attributeAnimation);
    }

    return true;
}

bool Animatable::Save(Serializer& dest) const
{
    if (!Serializable::Save(dest))
        return false;

    if (objectAnimation_ && objectAnimation_->GetName().Empty())
    {
        dest.WriteBool(true);
        if (!objectAnimation_->Save(dest))
            return false;
    }
    else
        dest.WriteBool(false);

    unsigned count = 0;
    for (HashMap<String, SharedPtr<AttributeAnimationInstance> >::ConstIterator i = attributeAnimationInstances_.Begin(); i != attributeAnimationInstances_.End(); ++i)
    {
        AttributeAnimation* attributeAnimation = i->second_->GetAttributeAnimation();
        if (attributeAnimation->GetObjectAnimation())
            continue;
        ++count;
    }

    dest.WriteUInt(count);
    for (HashMap<String, SharedPtr<AttributeAnimationInstance> >::ConstIterator i = attributeAnimationInstances_.Begin(); i != attributeAnimationInstances_.End(); ++i)
    {
        AttributeAnimation* attributeAnimation = i->second_->GetAttributeAnimation();
        if (attributeAnimation->GetObjectAnimation())
            continue;

        const AttributeInfo& attr = i->second_->GetAttributeInfo();
        dest.WriteString(attr.name_);
        if (!attributeAnimation->Save(dest))
            return false;
    }

    return true;
}

bool Animatable::LoadXML(const XMLElement& source, bool setInstanceDefault)
{
    if (!Serializable::LoadXML(source, setInstanceDefault))
        return false;
    
    XMLElement elem = source.GetChild("ObjectAnimation");
    if (elem)
    {
        SharedPtr<ObjectAnimation> objectAnimation(new ObjectAnimation(context_));
        if (!objectAnimation->LoadXML(elem))
            return false;

        SetObjectAnimation(objectAnimation);
    }

    elem = source.GetChild("AttributeAnimation");
    while (elem)
    {
        String name = elem.GetAttribute("name");
        SharedPtr<AttributeAnimation> attributeAnimation(new AttributeAnimation(context_));
        if (!attributeAnimation->LoadXML(elem))
            return false;

        SetAttributeAnimation(name, attributeAnimation);
        elem = elem.GetNext("AttributeAnimation");
    }

    return true;
}

bool Animatable::SaveXML(XMLElement& dest) const
{
    if (!Serializable::SaveXML(dest))
        return false;

    // Object animation without name
    if (objectAnimation_ && objectAnimation_->GetName().Empty())
    {
        XMLElement elem = dest.CreateChild("objectAnimation");
        if (!objectAnimation_->SaveXML(elem))
            return false;
    }

    for (HashMap<String, SharedPtr<AttributeAnimationInstance> >::ConstIterator i = attributeAnimationInstances_.Begin(); i != attributeAnimationInstances_.End(); ++i)
    {
        AttributeAnimation* attributeAnimation = i->second_->GetAttributeAnimation();
        if (attributeAnimation->GetObjectAnimation())
            continue;

        const AttributeInfo& attr = i->second_->GetAttributeInfo();
        XMLElement elem = dest.CreateChild("attributeAnimation");
        elem.SetAttribute("name", attr.name_);
        if (!attributeAnimation->SaveXML(elem))
            return false;
    }

    return true;
}

void Animatable::SetAnimationEnabled(bool animationEnabled)
{
    animationEnabled_ = animationEnabled;
}

void Animatable::SetObjectAnimation(ObjectAnimation* objectAnimation)
{
    if (objectAnimation == objectAnimation_)
        return;

    if (objectAnimation_)
        OnObjectAnimationRemoved(objectAnimation_);

    objectAnimation_ = objectAnimation;

    if (objectAnimation_)
        OnObjectAnimationAdded(objectAnimation_);
}

void Animatable::SetAttributeAnimation(const String& name, AttributeAnimation* attributeAnimation)
{
    const AttributeAnimationInstance* currentInstance = GetAttributeAnimationInstance(name);

    if (attributeAnimation)
    {
        if (currentInstance && attributeAnimation == currentInstance->GetAttributeAnimation())
            return;

        // Get attribute info
        const AttributeInfo* attributeInfo = 0;
        if (currentInstance)
            attributeInfo = &currentInstance->GetAttributeInfo();
        else
        {
            const Vector<AttributeInfo>* attributes = GetAttributes();
            if (!attributes)
            {
                LOGERROR(GetTypeName() + " has no attributes");
                return;
            }

            for (Vector<AttributeInfo>::ConstIterator i = attributes->Begin(); i != attributes->End(); ++i)
            {
                if (name == (*i).name_)
                {
                    attributeInfo = &(*i);
                    break;
                }
            }
        }

        if (!attributeInfo)
        {
            LOGERROR("Invalid name: " + name);
            return;
        }

        // Check value type is same with attribute type
        if (attributeAnimation->GetValueType() != attributeInfo->type_)
        {
            LOGERROR("Invalid value type");
            return;
        }

        // Add network attribute to set
        if (attributeInfo->mode_ & AM_NET)
        {
            const Vector<AttributeInfo>* networkAttributes = GetNetworkAttributes();
            for (Vector<AttributeInfo>::ConstIterator i = networkAttributes->Begin(); i != networkAttributes->End(); ++i)
            {
                if (name == (*i).name_)
                {
                    animatedNetworkAttributes_.Insert(&(*i));
                    break;
                }
            }            
        }

        attributeAnimationInstances_[name] = new AttributeAnimationInstance(this, *attributeInfo, attributeAnimation);

        if (!currentInstance)
            OnAttributeAnimationAdded();
    }
    else
    {
        if (!currentInstance)
            return;

        // Remove network attribute from set
        if (currentInstance->GetAttributeInfo().mode_ & AM_NET)
        {
            const Vector<AttributeInfo>* networkAttributes = GetNetworkAttributes();
            for (Vector<AttributeInfo>::ConstIterator i = networkAttributes->Begin(); i != networkAttributes->End(); ++i)
            {
                if (name == (*i).name_)
                {
                    animatedNetworkAttributes_.Erase(&(*i));
                    break;
                }
            }            
        }

        attributeAnimationInstances_.Erase(name);
        OnAttributeAnimationRemoved();
    }
}

const ObjectAnimation* Animatable::GetObjectAnimation() const
{
    return objectAnimation_;
}

const AttributeAnimation* Animatable::GetAttributeAnimation(const String& name) const
{
    const AttributeAnimationInstance* instance = GetAttributeAnimationInstance(name);
    return instance ? instance->GetAttributeAnimation() : 0;
}

void Animatable::SetObjectAnimationAttr(ResourceRef value)
{
    if (!value.name_.Empty())
    {
        ResourceCache* cache = GetSubsystem<ResourceCache>();
        SetObjectAnimation(cache->GetResource<ObjectAnimation>(value.name_));
    }
}

ResourceRef Animatable::GetObjectAnimationAttr() const
{
    return GetResourceRef(objectAnimation_, ObjectAnimation::GetTypeStatic());
}

void Animatable::OnObjectAnimationAdded(ObjectAnimation* objectAnimation)
{
    if (!objectAnimation)
        return;

    // Set all attribute animations from the object animation
    HashMap<String, SharedPtr<AttributeAnimation> > attributeAnimations = objectAnimation->GetAttributeAnimations();
    for (HashMap<String, SharedPtr<AttributeAnimation> >::Iterator i = attributeAnimations.Begin(); i != attributeAnimations.End(); ++i)
        SetAttributeAnimation(i->first_, i->second_);
}

void Animatable::OnObjectAnimationRemoved(ObjectAnimation* objectAnimation)
{
    if (!objectAnimation)
        return;

    // Just remove all attribute animations from the object animation
    Vector<String> names;
    for (HashMap<String, SharedPtr<AttributeAnimationInstance> >::Iterator i = attributeAnimationInstances_.Begin(); i != attributeAnimationInstances_.End(); ++i)
    {
        if (i->second_->GetAttributeAnimation()->GetObjectAnimation() == objectAnimation)
            names.Push(i->first_);
    }

    for (unsigned int i = 0; i < names.Size(); ++i)
        SetAttributeAnimation(names[i], 0);
}

void Animatable::UpdateAttributeAnimations(float timeStep)
{
    if (!animationEnabled_)
        return;
    
    for (HashMap<String, SharedPtr<AttributeAnimationInstance> >::ConstIterator i = attributeAnimationInstances_.Begin(); i != attributeAnimationInstances_.End(); ++i)
        i->second_->Update(timeStep);
}

bool Animatable::IsAnimatedNetworkAttribute(const AttributeInfo& attrInfo) const
{
    return animatedNetworkAttributes_.Find(&attrInfo) != animatedNetworkAttributes_.End();
}

const AttributeAnimationInstance* Animatable::GetAttributeAnimationInstance(const String& name) const
{
    HashMap<String, SharedPtr<AttributeAnimationInstance> >::ConstIterator i = attributeAnimationInstances_.Find(name);
    if (i != attributeAnimationInstances_.End())
        return i->second_;

    return 0;
}

}