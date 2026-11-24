local q=Quaternion.identity()
assert(q.w==1 and q.x==0 and q.y==0 and q.z==0)
assert(Vector3f.new(0,3,4):length()==5)
for _,name in ipairs({"UpdateScene","LateUpdateBehavior","BeginRendering"}) do
    local before,after=0,0
    re.on_pre_application_entry(name,function()
        before=before+1
    end)
    re.on_application_entry(name,function()
        after=after+1
        if after==60 then
            log.info(string.format("via.Application %s pre=%d post=%d",name,before,after))
        end
    end)
end
